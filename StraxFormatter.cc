#include "StraxFormatter.hh"
#include "DAQController.hh"
#include "MongoLog.hh"
#include "Options.hh"
#include "V1724.hh"
#include <lz4frame.h>
#include <blosc.h>
#include <thread>
#include <cstring>
#include <cstdarg>
#include <numeric>
#include <sstream>
#include <bitset>
#include <iomanip>
#include <ctime>
#include <cmath>

namespace fs=std::experimental::filesystem;
using namespace std::chrono;
const int event_header_words = 4, max_channels = 16;

double timespec_subtract(struct timespec& a, struct timespec& b) {
  return (a.tv_sec - b.tv_sec)*1e6 + (a.tv_nsec - b.tv_nsec)/1e3;
}

StraxFormatter::StraxFormatter(std::shared_ptr<Options>& opts, std::shared_ptr<MongoLog>& log){
  fActive = true;
  fChunkNameLength=6;
  fStraxHeaderSize=24;
  fBytesProcessed = 0;
  fInputBufferSize = 0;
  fOutputBufferSize = 0;
  fProcTimeDP = fProcTimeEv = fProcTimeCh = fCompTime = 0.;
  fOptions = opts;
  fChunkLength = long(fOptions->GetDouble("strax_chunk_length", 5)*1e9); // default 5s
  fChunkOverlap = long(fOptions->GetDouble("strax_chunk_overlap", 0.5)*1e9); // default 0.5s
  fFragmentBytes = fOptions->GetInt("strax_fragment_payload_bytes", 110*2);
  fCompressor = fOptions->GetString("compressor", "lz4");
  fFullChunkLength = fChunkLength+fChunkOverlap;
  fHostname = fOptions->Hostname();
  std::string run_name = fOptions->GetString("run_identifier", "run");

  fEmptyVerified = 0;
  fLog = log;

  fBufferNumChunks = fOptions->GetInt("strax_buffer_num_chunks", 2);
  fWarnIfChunkOlderThan = fOptions->GetInt("strax_chunk_phase_limit", 2);

  std::string output_path = fOptions->GetString("strax_output_path", "./");
  try{
    fs::path op(output_path);
    op /= run_name;
    fOutputPath = op;
    fs::create_directory(op);
  }
  catch(...){
    fLog->Entry(MongoLog::Error, "StraxFormatter::Initialize tried to create output directory but failed. Check that you have permission to write here.");
    throw std::runtime_error("No write permissions");
  }
}

StraxFormatter::~StraxFormatter(){
  std::stringstream ss;
  ss << std::hex << fThreadId;
  std::map<std::string, double> times {
    {"data_packets_us", fProcTimeDP},
    {"events_us", fProcTimeEv},
    {"fragments_us", fProcTimeCh},
    {"compression_us", fCompTime}
  };
  std::map<std::string, std::map<int, long>> counters {
    {"fragments", fFragsPerEvent},
    {"events", fEvPerDP},
    {"data_packets", fBufferCounter},
    {"chunks", fBytesPerChunk}
  };
  fOptions->SaveBenchmarks(counters, fBytesProcessed, ss.str(), times);
}

void StraxFormatter::Close(std::map<int,int>& ret){
  fActive = false;
  for (auto& iter : fFailCounter) ret[iter.first] += iter.second;
  fCV.notify_one();
}

void StraxFormatter::GetDataPerChan(std::map<int, int>& ret) {
  if (!fActive) return;
  const std::lock_guard<std::mutex> lk(fDPC_mutex);
  for (auto& pair : fDataPerChan) {
    ret[pair.first] += pair.second;
    pair.second = 0;
  }
  return;
}

void StraxFormatter::GenerateArtificialDeadtime(int64_t timestamp, const std::shared_ptr<V1724>& digi) {
  std::string fragment;
  fragment.reserve(fFragmentBytes + fStraxHeaderSize);
  timestamp *= digi->GetClockWidth();
  fragment.append((char*)&timestamp, sizeof(timestamp));
  int32_t length = fFragmentBytes>>1;
  fragment.append((char*)&length, sizeof(length));
  int16_t sw = 10;
  fragment.append((char*)&sw, sizeof(sw));
  int16_t channel = 790; // TODO add MV and NV support
  fragment.append((char*)&channel, sizeof(channel));
  fragment.append((char*)&length, sizeof(length));
  int16_t fragment_i = 0;
  fragment.append((char*)&fragment_i, sizeof(fragment_i));
  int16_t baseline = 0;
  fragment.append((char*)&baseline, sizeof(baseline));
  int8_t zero = 0;
  while ((int)fragment.size() < fFragmentBytes+fStraxHeaderSize)
    fragment.append((char*)&zero, sizeof(zero));
  AddFragmentToBuffer(std::move(fragment), 0, 0);
}

void StraxFormatter::ProcessDatapacket(std::unique_ptr<data_packet> dp){
  // Take a buffer and break it up into one document per channel
  struct timespec dp_start, dp_end, ev_start, ev_end;
  auto it = dp->buff.begin();
  int evs_this_dp(0), words(0);
  bool missed = false;
  std::map<int, int> dpc;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &dp_start);
  do {
    if((*it)>>28 == 0xA){
      missed = true; // it works out
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ev_start);
      words = (*it)&0xFFFFFFF;
      fLog->Entry(MongoLog::Local, "Bd %i %x/%x/%x", dp->digi->bid(),
          std::distance(dp->buff.begin(), it), words, dp->buff.size());
      std::u32string_view sv(dp->buff.data() + std::distance(dp->buff.begin(), it), words);
      ProcessEvent(sv, dp, dpc);
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ev_end);
      fProcTimeEv += timespec_subtract(ev_end, ev_start);
      evs_this_dp++;
      it += words;
    } else {
      if (missed) {
        fLog->Entry(MongoLog::Warning, "Missed an event from %i at idx %i",
            dp->digi->bid(), std::distance(dp->buff.begin(), it));
        missed = false;
      }
      it++;
    }
  } while (it < dp->buff.end() && fActive == true);
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &dp_end);
  fProcTimeDP += timespec_subtract(dp_end, dp_start);
  fBytesProcessed += dp->buff.size()*sizeof(char32_t);
  fEvPerDP[evs_this_dp]++;
  {
    const std::lock_guard<std::mutex> lk(fDPC_mutex);
    for (auto& p : dpc) fDataPerChan[p.first] += p.second;
  }
  fInputBufferSize -= dp->buff.size()*sizeof(char32_t);
}

int StraxFormatter::ProcessEvent(std::u32string_view buff,
    const std::unique_ptr<data_packet>& dp, std::map<int, int>& dpc) {
  // buff = start of event

  struct timespec ch_start, ch_end;

  // returns {words this event, channel mask, board fail, header timestamp}
  auto [words, channel_mask, fail, event_time] = dp->digi->UnpackEventHeader(buff);

  if(fail){ // board fail
    GenerateArtificialDeadtime(((dp->clock_counter<<31) + dp->header_time), dp->digi);
    dp->digi->CheckFail(true);
    fFailCounter[dp->digi->bid()]++;
    return event_header_words;
  }

  buff.remove_prefix(event_header_words);
  int ret;
  int frags(0);

  for(unsigned ch=0; ch<max_channels; ch++){
    if (channel_mask & (1<<ch)) {
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ch_start);
      ret = ProcessChannel(buff, words, channel_mask, event_time, frags, ch, dp, dpc);
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ch_end);
      fProcTimeCh += timespec_subtract(ch_end, ch_start);
      if (ret == -1)
        break;
      buff.remove_prefix(ret);
    }
  }
  fFragsPerEvent[frags]++;
  return words;
}

int StraxFormatter::ProcessChannel(std::u32string_view buff, int words_in_event,
    int channel_mask, uint32_t event_time, int& frags, int channel,
    const std::unique_ptr<data_packet>& dp, std::map<int, int>& dpc) {
  // buff points to the first word of the channel's data

  int n_channels = std::bitset<max_channels>(channel_mask).count();
  // returns {timestamp (ns), words this channel, baseline, waveform}
  auto [timestamp, channel_words, baseline_ch, wf] = dp->digi->UnpackChannelHeader(
      buff, dp->clock_counter, dp->header_time, event_time, words_in_event, n_channels);

  uint32_t samples_in_pulse = wf.size()*sizeof(uint16_t)/sizeof(char32_t);
  uint16_t sw = dp->digi->SampleWidth();
  int samples_per_frag= fFragmentBytes>>1;
  int16_t global_ch = fOptions->GetChannel(dp->digi->bid(), channel);
  // Failing to discern which channel we're getting data from seems serious enough to throw
  if(global_ch==-1)
    throw std::runtime_error("Failed to parse channel map. I'm gonna just kms now.");
  //fLog->Entry(MongoLog::Local, "%i/%i (%i) %lx %x %x %i", dp->digi->bid(), channel,
  //    global_ch, timestamp, dp->header_time, event_time, dp->clock_counter);

  int num_frags = std::ceil(1.*samples_in_pulse/samples_per_frag);
  frags += num_frags;
  for (uint16_t frag_i = 0; frag_i < num_frags; frag_i++) {
    std::string fragment;
    fragment.reserve(fFragmentBytes + fStraxHeaderSize);

    // How long is this fragment?
    uint32_t samples_this_frag = samples_per_frag;
    if (frag_i == num_frags-1)
      samples_this_frag = samples_in_pulse - frag_i*samples_per_frag;

    int64_t time_this_frag = timestamp + samples_per_frag*sw*frag_i;
    fragment.append((char*)&time_this_frag, sizeof(time_this_frag));
    fragment.append((char*)&samples_this_frag, sizeof(samples_this_frag));
    fragment.append((char*)&sw, sizeof(sw));
    fragment.append((char*)&global_ch, sizeof(global_ch));
    fragment.append((char*)&samples_in_pulse, sizeof(samples_in_pulse));
    fragment.append((char*)&frag_i, sizeof(frag_i));
    fragment.append((char*)&baseline_ch, sizeof(baseline_ch));

    // Copy the raw buffer
    fragment.append((char*)wf.data(), samples_this_frag*sizeof(uint16_t));
    wf.remove_prefix(samples_this_frag*sizeof(uint16_t)/sizeof(char32_t));
    uint16_t zero_filler = 0;
    while((int)fragment.size()<fFragmentBytes+fStraxHeaderSize)
      fragment.append((char*)&zero_filler, sizeof(zero_filler));

    AddFragmentToBuffer(std::move(fragment), event_time, dp->clock_counter);
  } // loop over frag_i
  dpc[global_ch] += samples_in_pulse*sizeof(uint16_t);
  return channel_words;
}

void StraxFormatter::AddFragmentToBuffer(std::string fragment, uint32_t ts, int rollovers) {
  // Get the CHUNK and decide if this event also goes into a PRE/POST file
  int64_t timestamp = *(int64_t*)fragment.data();
  int chunk_id = timestamp/fFullChunkLength;
  bool overlap = (chunk_id+1)* fFullChunkLength - timestamp <= fChunkOverlap;
  int min_chunk(0), max_chunk(1);
  if (fChunks.size() > 0) {
    auto [min_iter, max_iter] = std::minmax_element(fChunks.begin(), fChunks.end(), 
      [&](auto& l, auto& r) {return l.first < r.first;});
    min_chunk = (*min_iter).first;
    max_chunk = (*max_iter).first;
  }

  if (min_chunk - chunk_id > fWarnIfChunkOlderThan) {
    const short* channel = (const short*)(fragment.data()+14);
    fLog->Entry(MongoLog::Warning,
        "Thread %lx got data from ch %i that's in chunk %i instead of %i/%i (ts %lx), it might get lost (ts %lx ro %i)",
        fThreadId, *channel, chunk_id, min_chunk, max_chunk, timestamp, ts, rollovers);
  } else if (chunk_id - max_chunk > 1) {
    fLog->Entry(MongoLog::Message, "Thread %lx skipped %i chunk(s)",
        fThreadId, chunk_id - max_chunk - 1);
  }

  fOutputBufferSize += fragment.size();

  if(!overlap){
    fChunks[chunk_id].emplace_back(std::move(fragment));
  } else {
    fOverlaps[chunk_id].emplace_back(std::move(fragment));
  }
}

void StraxFormatter::ReceiveDatapackets(std::list<std::unique_ptr<data_packet>>& in) {
  {
    const std::lock_guard<std::mutex> lk(fBufferMutex);
    fBufferCounter[in.size()]++;
    fBuffer.splice(fBuffer.end(), in);
  }
  fCV.notify_one();
}

void StraxFormatter::Process() {
  // this func runs in its own thread
  fThreadId = std::this_thread::get_id();
  std::stringstream ss;
  ss<<fHostname<<'_'<<fThreadId;
  fFullHostname = ss.str();
  fActive = fRunning = true;
  std::unique_ptr<data_packet> dp;
  while (fActive == true) {
    std::unique_lock<std::mutex> lk(fBufferMutex);
    fCV.wait(lk, [&]{return fBuffer.size() > 0 || fActive == false;});
    if (fBuffer.size() > 0) {
      dp = std::move(fBuffer.front());
      fBuffer.pop_front();
      lk.unlock();
      ProcessDatapacket(std::move(dp));
      WriteOutChunks();
    } else {
      lk.unlock();
    }
  }
  if (fBytesProcessed > 0)
    End();
  fRunning = false;
}

// Can tune here as needed, these are defaults from the LZ4 examples
static const LZ4F_preferences_t kPrefs = {
  { LZ4F_max256KB, LZ4F_blockLinked, LZ4F_noContentChecksum, LZ4F_frame, 0, { 0, 0 } },
    0,   /* compression level; 0 == default */
    0,   /* autoflush */
    { 0, 0, 0 },  /* reserved, must be set to 0 */
};

void StraxFormatter::WriteOutChunk(int chunk_i){
  // Write the contents of the buffers to compressed files
  struct timespec comp_start, comp_end;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &comp_start);

  std::vector<std::list<std::string>*> buffers = {&fChunks[chunk_i], &fOverlaps[chunk_i]};
  std::vector<size_t> uncompressed_size(3, 0);
  std::string uncompressed;
  std::vector<std::shared_ptr<std::string>> out_buffer(3);
  std::vector<int> wsize(3);
  size_t max_compressed_size = 0;

  for (int i = 0; i < 2; i++) {
    uncompressed_size[i] = buffers[i]->size()*(fFragmentBytes + fStraxHeaderSize);
    uncompressed.reserve(uncompressed_size[i]);
    for (auto it = buffers[i]->begin(); it != buffers[i]->end(); it++)
      uncompressed += *it;
    buffers[i]->clear();
    if(fCompressor == "blosc"){
      max_compressed_size = uncompressed_size[i] + BLOSC_MAX_OVERHEAD;
      out_buffer[i] = std::make_shared<std::string>(max_compressed_size, 0);
      wsize[i] = blosc_compress_ctx(5, 1, sizeof(char), uncompressed_size[i],
          uncompressed.data(), out_buffer[i]->data(), max_compressed_size,"lz4", 0, 2);
    }else{
      // Note: the current package repo version for Ubuntu 18.04 (Oct 2019) is 1.7.1, which is
      // so old it is not tracked on the lz4 github. The API for frame compression has changed
      // just slightly in the meantime. So if you update and it breaks you'll have to tune at least
      // the LZ4F_preferences_t object to the new format.
      max_compressed_size = LZ4F_compressFrameBound(uncompressed_size[i], &kPrefs);
      out_buffer[i] = std::make_shared<std::string>(max_compressed_size, 0);
      wsize[i] = LZ4F_compressFrame(out_buffer[i]->data(), max_compressed_size,
          uncompressed.data(), uncompressed_size[i], &kPrefs);
    }
    uncompressed.clear();
    fBytesPerChunk[int(std::log2(uncompressed_size[i]))]++;
    fOutputBufferSize -= uncompressed_size[i];
  }
  fChunks.erase(chunk_i);
  fOverlaps.erase(chunk_i);

  out_buffer[2] = out_buffer[1];
  wsize[2] = wsize[1];
  uncompressed_size[2] = uncompressed_size[1];
  std::vector<std::string> names {{GetStringFormat(chunk_i),
    GetStringFormat(chunk_i)+"_post", GetStringFormat(chunk_i+1)+"_pre"}};
  for (int i = 0; i < 3; i++) {
    // write to *_TEMP
    auto output_dir_temp = GetDirectoryPath(names[i], true);
    auto filename_temp = GetFilePath(names[i], true);
    if (!fs::exists(output_dir_temp))
      fs::create_directory(output_dir_temp);
    std::ofstream writefile(filename_temp, std::ios::binary);
    if (uncompressed_size[i] > 0) writefile.write(out_buffer[i]->data(), wsize[i]);
    writefile.close();
    out_buffer[i].reset();

    auto output_dir = GetDirectoryPath(names[i]);
    auto filename = GetFilePath(names[i]);
    // shenanigans or skulduggery?
    if(fs::exists(filename)) {
      fLog->Entry(MongoLog::Warning, "Chunk %s from thread %lx already exists? %li vs %li bytes",
          names[i].c_str(), fThreadId, fs::file_size(filename), wsize[i]);
    }

    // Move this chunk from *_TEMP to the same path without TEMP
    if(!fs::exists(output_dir))
      fs::create_directory(output_dir);
    fs::rename(filename_temp, filename);
  } // End writing
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &comp_end);
  fCompTime += timespec_subtract(comp_end, comp_start);
  return;
}

void StraxFormatter::WriteOutChunks() {
  if ((int)fChunks.size() < fBufferNumChunks) return;
  auto [min_iter, max_iter] = std::minmax_element(fChunks.begin(), fChunks.end(),
      [&](auto& a, auto& b){return a.first < b.first;});
  int max_chunk = (*max_iter).first;
  int min_chunk = (*min_iter).first;
  for (; min_chunk <= max_chunk - fBufferNumChunks; min_chunk++)
    WriteOutChunk(min_chunk);
  CreateEmpty(min_chunk);
  return;
}

void StraxFormatter::End() {
  for (auto& p : fChunks)
    WriteOutChunk(p.first);
  fChunks.clear();
  auto end_dir = GetDirectoryPath("THE_END");
  if(!fs::exists(end_dir)){
    fLog->Entry(MongoLog::Local,"Creating END directory at %s", end_dir.c_str());
    try{
      fs::create_directory(end_dir);
    }
    catch(...){};
  }
  std::ofstream outfile(GetFilePath("THE_END"), std::ios::out);
  outfile<<"...my only friend";
  outfile.close();
  return;
}

std::string StraxFormatter::GetStringFormat(int id){
  std::string chunk_index = std::to_string(id);
  while(chunk_index.size() < fChunkNameLength)
    chunk_index.insert(0, "0");
  return chunk_index;
}

fs::path StraxFormatter::GetDirectoryPath(const std::string& id, bool temp){
  fs::path write_path(fOutputPath);
  write_path /= id;
  if(temp)
    write_path+="_temp";
  return write_path;
}

fs::path StraxFormatter::GetFilePath(const std::string& id, bool temp){
  return GetDirectoryPath(id, temp) / fFullHostname;
}

void StraxFormatter::CreateEmpty(int back_from){
  for(; fEmptyVerified<back_from; fEmptyVerified++){
    std::vector<std::string> names {{GetStringFormat(fEmptyVerified),
      GetStringFormat(fEmptyVerified)+"_post", GetStringFormat(fEmptyVerified+1)+"_pre"}};
    for (auto& n : names) {
      if(!fs::exists(GetFilePath(n))){
        if(!fs::exists(GetDirectoryPath(n)))
          fs::create_directory(GetDirectoryPath(n));
        std::ofstream o(GetFilePath(n));
        o.close();
      }
    } // name
  } // chunks
}

