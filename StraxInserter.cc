#include "StraxInserter.hh"
#include <lz4frame.h>
#include "DAQController.hh"
#include "MongoLog.hh"
#include "Options.hh"
#include <blosc.h>
#include <thread>
#include <cstring>
#include <cstdarg>
#include <numeric>
#include <sstream>
#include <list>
#include <bitset>
#include <iomanip>

namespace fs=std::experimental::filesystem;
using namespace std::chrono;
const int event_header_words = 4, max_channels = 16;

StraxInserter::StraxInserter(){
  fOptions = NULL;
  fDataSource = NULL;
  fActive = true;
  fChunkLength=0x7fffffff; // DAQ magic number
  fChunkNameLength=6;
  fChunkOverlap = 0x2FAF080;
  fStraxHeaderSize=24;
  fFragmentBytes=110*2;
  fLog = NULL;
  fErrorBit = false;
  fMissingVerified = 0;
  fOutputPath = "";
  fThreadId = std::this_thread::get_id();
  fBytesProcessed = 0;
  fFragmentSize = 0;
  fForceQuit = false;
  fFullChunkLength = fChunkLength+fChunkOverlap;
  fFragmentsProcessed = 0;
  fEventsProcessed = 0;
}

StraxInserter::~StraxInserter(){
  fActive = false;
  int counter_short = 0, counter_long = 0;
  if (fBufferLength.load() > 0)
    fLog->Entry(MongoLog::Local, "Thread %lx waiting to stop, has %i events left",
        fThreadId, fBufferLength.load());
  else
    fLog->Entry(MongoLog::Local, "Thread %lx stopping", fThreadId);
  int events_start = fBufferLength.load();
  do{
    events_start = fBufferLength.load();
    while (fRunning && counter_short++ < 500)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (counter_short >= 500)
      fLog->Entry(MongoLog::Message, "Thread %lx taking a while to stop, still has %i evts",
          fThreadId, fBufferLength.load());
    counter_short = 0;
  } while (fRunning && fBufferLength.load() > 0 && events_start > fBufferLength.load() && counter_long++ < 10);
  if (fRunning) {
    fLog->Entry(MongoLog::Warning, "Force-quitting thread %lx: %i events lost",
        fThreadId, fBufferLength.load());
    fForceQuit = true;
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  while (fRunning) {
    fLog->Entry(MongoLog::Message, "Still waiting for thread %lx to stop", fThreadId);
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  long total_dps = std::accumulate(fBufferCounter.begin(), fBufferCounter.end(), 0,
      [&](long tot, auto& p){return tot + p.second;});
  std::map<std::string, long> counters {
    {"bytes", fBytesProcessed},
    {"fragments", fFragmentsProcessed},
    {"events", fEventsProcessed},
    {"data_packets", total_dps}};
  fOptions->SaveBenchmarks(counters, fBufferCounter,
      fProcTimeDP.count(), fProcTimeEv.count(), fProcTimeCh.count(), fCompTime.count());
  fLog->Entry(MongoLog::Local, "Thread %lx did%s see bit[30]", fThreadId, fSawBit30 ? "" : " not");
}

int StraxInserter::Initialize(Options *options, MongoLog *log, DAQController *dataSource,
			      std::string hostname){
  fOptions = options;
  fChunkLength = long(fOptions->GetDouble("strax_chunk_length", 5)*1e9); // default 5s
  fChunkOverlap = long(fOptions->GetDouble("strax_chunk_overlap", 0.5)*1e9); // default 0.5s
  fFragmentBytes = fOptions->GetInt("strax_fragment_payload_bytes", 110*2);
  fCompressor = fOptions->GetString("compressor", "lz4");
  fFullChunkLength = fChunkLength+fChunkOverlap;
  fHostname = hostname;
  std::string run_name = fOptions->GetString("run_identifier", "run");

  fMissingVerified = 0;
  fDataSource = dataSource;
  dataSource->GetDataFormat(fFmt);
  fLog = log;
  fErrorBit = false;

  fProcTimeDP = fProcTimeEv = fProcTimeCh = fCompTime = microseconds(0);
  fBufferNumChunks = fOptions->GetInt("strax_buffer_num_chunks", 2);

  std::string output_path = fOptions->GetString("strax_output_path", "./");
  try{
    fs::path op(output_path);
    op /= run_name;
    fOutputPath = op;
    fs::create_directory(op);
  }
  catch(...){
    fLog->Entry(MongoLog::Error, "StraxInserter::Initialize tried to create output directory but failed. Check that you have permission to write here.");
    return -1;
  }

  fSawBit30 = false;
  return 0;
}

void StraxInserter::Close(std::map<int,int>& ret){
  fActive = false;
  const std::lock_guard<std::mutex> lg(fFC_mutex);
  for (auto& iter : fFailCounter) ret[iter.first] += iter.second;
}

void StraxInserter::GetDataPerChan(std::map<int, int>& ret) {
  if (!fActive) return;
  fDPC_mutex.lock();
  for (auto& pair : fDataPerChan) {
    ret[pair.first] += pair.second;
    pair.second = 0;
  }
  fDPC_mutex.unlock();
  return;
}

void StraxInserter::GenerateArtificialDeadtime(int64_t timestamp, int16_t bid) {
  std::string fragment;
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
  fragment.append((char*)&bid, sizeof(bid));
  int8_t zero = 0;
  while ((int)fragment.size() < fFragmentBytes+fStraxHeaderSize)
    fragment.append((char*)&zero, sizeof(zero));
  AddFragmentToBuffer(fragment, timestamp);
}

void StraxInserter::ProcessDatapacket(data_packet* dp){

  system_clock::time_point proc_start, proc_end, ev_start, ev_end;

  // Take a buffer and break it up into one document per channel

  u_int32_t *buff = dp->buff;
  u_int32_t idx = 0;
  unsigned total_words = dp->size/sizeof(u_int32_t);
  proc_start = system_clock::now();
  while(idx < total_words){

    if(buff[idx]>>28 == 0xA){ // 0xA indicates header at those bits
      ev_start = system_clock::now();
      idx += ProcessEvent(buff+idx, total_words-idx, dp->clock_counter, dp->header_time, dp->bid);
      ev_end = system_clock::now();
      fProcTimeEv += duration_cast<microseconds>(ev_end - ev_start);
    } else
      idx++;
    if (fForceQuit) break;
  }
  proc_end = system_clock::now();
  fProcTimeDP += duration_cast<microseconds>(proc_end - proc_start);

  delete dp;
}

uint32_t StraxInserter::ProcessEvent(uint32_t* buff, unsigned total_words, long clock_counter,
    uint32_t header_time, int bid) {
  // buff = start of event, total_words = valid words remaining in total buffer

  system_clock::time_point proc_start, proc_end;
  std::map<std::string, int> fmt = fFmt[bid];

  u_int32_t words_in_event = std::min(buff[0]&0xFFFFFFF, total_words);
  if (words_in_event < (buff[0]&0xFFFFFFF)) {
    fLog->Entry(MongoLog::Local, "Board %i garbled event header: %u/%u",
        bid, buff[0]&0xFFFFFFF, total_words);
  }

  u_int32_t channel_mask = (buff[1]&0xFF);
  if (fmt["channel_mask_msb_idx"] != -1) channel_mask |= ( ((buff[2]>>24)&0xFF)<<8);

  u_int32_t event_time = buff[3]&0xFFFFFFFF;
  fEventsProcessed++;

  if(buff[1]&0x4000000){ // board fail
    const std::lock_guard<std::mutex> lg(fFC_mutex);
    GenerateArtificialDeadtime(((clock_counter<<31) + header_time)*fmt["ns_per_clock"], bid);
    fDataSource->CheckError(bid);
    fFailCounter[bid]++;
    return event_header_words;
  }

  unsigned idx = event_header_words;
  int ret;

  for(unsigned ch=0; ch<max_channels; ch++){
    if (channel_mask & (1<<ch)) {
      proc_start = system_clock::now();
      ret = ProcessChannel(buff+idx, words_in_event, bid, ch, header_time, event_time,
        clock_counter, channel_mask);
      proc_end = system_clock::now();
      fProcTimeCh += duration_cast<microseconds>(proc_end - proc_start);
      if (ret == -1)
        break;
      idx += ret;
    }
  }
  return idx;
}

int StraxInserter::ProcessChannel(uint32_t* buff, unsigned words_in_event, int bid, int channel,
    uint32_t header_time, uint32_t event_time, long clock_counter, int channel_mask) {
  // buff points to the first word of the channel's data

  // These defaults are valid for 'default' firmware where all channels are the same size
  int channels_in_event = std::bitset<max_channels>(channel_mask).count();
  u_int32_t channel_words = (words_in_event-event_header_words) / channels_in_event;
  long channel_time = (clock_counter<<31) + event_time;
  long channel_timeMSB = 0;
  u_int16_t baseline_ch = 0;
  std::map<std::string, int> fmt = fFmt[bid];

  // Presence of a channel header indicates non-default firmware (DPP-DAW) so override
  if(fmt["channel_header_words"] > 0){
    channel_words = std::min(buff[0]&0x7FFFFF, words_in_event);
    if (channel_words < (buff[0]&0x7FFFFF)) {
      fLog->Entry(MongoLog::Local, "Board %i ch %i garbled header: %x/%x",
                  bid, channel, buff[0]&0x7FFFFF, words_in_event);
      return -1;
    }
    if ((int)channel_words <= fmt["channel_header_words"]) {
      fLog->Entry(MongoLog::Local, "Board %i ch %i empty (%i/%i)",
            bid, channel, channel_words, fmt["channel_header_words"]);
      return -1;
    }
    channel_time = buff[1]&0xFFFFFFFF;
    fSawBit30 |= (channel_time & (1 << 30));

    if (fmt["channel_time_msb_idx"] == 2) {
      channel_timeMSB = long(buff[2]&0xFFFF)<<32;
      baseline_ch = (buff[2]>>16)&0x3FFF;
    }

    if(fmt["channel_header_words"] <= 2){
      // More clock rollover logic here, because channels are independent
      // and we process multithreaded. We leverage the fact that readout windows are short
      // and polled frequently compared to the clock rollover timescale, so there will
      // never be a "large" difference in realtime between timestamps in a data_packet

      // first, has the main counter rolled but this channel hasn't?
      if(channel_time>15e8 && header_time<5e8 && clock_counter!=0){
        clock_counter--;
      }
      // Now check the opposite
      else if(channel_time<5e8 && header_time>15e8){
        clock_counter++;
      }
      channel_timeMSB = clock_counter<<31;
    }
  } // channel_header_words > 0

  int64_t Time64 = fmt["ns_per_clk"]*(channel_timeMSB + channel_time); // in ns

  // let's sanity-check the data first to make sure we didn't get CAENed
  for (unsigned w = fmt["channel_header_words"]; w < channel_words; w++) {
    if ((buff[w]>>28) == 0xA) {
      fLog->Entry(MongoLog::Local, "Board %i has CAEN'd itself", bid);
      GenerateArtificialDeadtime(Time64, bid);
      return -1;
    }
  }

  u_int16_t *payload = reinterpret_cast<u_int16_t*>(buff+fmt["channel_header_words"]);
  u_int32_t samples_in_pulse = (channel_words-fmt["channel_header_words"])<<1;
  u_int16_t sw = fmt["ns_per_sample"];
  int fragment_samples = fFragmentBytes>>1;
  int16_t cl = fOptions->GetChannel(bid, channel);
  // Failing to discern which channel we're getting data from seems serious enough to throw
  if(cl==-1)
    throw std::runtime_error("Failed to parse channel map. I'm gonna just kms now.");

  int num_frags = samples_in_pulse/fragment_samples + (samples_in_pulse % fragment_samples ? 1 : 0);
  for (uint16_t frag_i = 0; frag_i < num_frags; frag_i++) {
    std::string fragment;

    // How long is this fragment?
    u_int32_t samples_this_fragment = fragment_samples;
    if (frag_i == num_frags-1)
      samples_this_fragment = samples_in_pulse - frag_i*fragment_samples;
    fFragmentsProcessed++;

    u_int64_t time_this_fragment = Time64 + fragment_samples*sw*frag_i;
    fragment.append((char*)&time_this_fragment, sizeof(time_this_fragment));
    fragment.append((char*)&samples_this_fragment, sizeof(samples_this_fragment));
    fragment.append((char*)&sw, sizeof(sw));
    fragment.append((char*)&cl, sizeof(cl));
    fragment.append((char*)&samples_in_pulse, sizeof(samples_in_pulse));
    fragment.append((char*)&frag_i, sizeof(frag_i));
    fragment.append((char*)&baseline_ch, sizeof(baseline_ch));

    // Copy the raw buffer
    fragment.append((char*)(payload + frag_i*fragment_samples), samples_this_fragment*2);
    uint16_t zero_filler = 0;
    while((int)fragment.size()<fFragmentBytes+fStraxHeaderSize)
      fragment.append((char*)&zero_filler, 2);

    AddFragmentToBuffer(fragment, time_this_fragment);
  } // loop over frag_i
  {
    const std::lock_guard<std::mutex> lg(fDPC_mutex);
    fDataPerChan[cl] += samples_in_pulse<<1;
  }
  return channel_words;
}

void StraxInserter::AddFragmentToBuffer(std::string& fragment, int64_t timestamp) {
  // Get the CHUNK and decide if this event also goes into a PRE/POST file
  int chunk_id = timestamp/fFullChunkLength;
  bool nextpre = (chunk_id+1)* fFullChunkLength - timestamp <= fChunkOverlap;
  // Minor mess to maintain the same width of file names and do the pre/post stuff
  // If not in pre/post
  std::string chunk_index = GetStringFormat(chunk_id);

  fFragmentSize += fragment.size();

  if(!nextpre){
    if(fFragments.count(chunk_index) == 0){
      fFragments[chunk_index] = new std::string();
    }
    fFragments[chunk_index]->append(fragment);
    fTimeLastSeen[chunk_index] = system_clock::now();
  } else {
    std::string nextchunk_index = GetStringFormat(chunk_id+1);

    if(fFragments.count(nextchunk_index+"_pre") == 0){
      fFragments[nextchunk_index+"_pre"] = new std::string();
    }
    fFragments[nextchunk_index+"_pre"]->append(fragment);
    fTimeLastSeen[nextchunk_index+"_pre"] = system_clock::now();

    if(fFragments.count(chunk_index+"_post") == 0){
      fFragments[chunk_index+"_post"] = new std::string();
    }
    fFragments[chunk_index+"_post"]->append(fragment);
    fTimeLastSeen[chunk_index+"_post"] = system_clock::now();
  }
}

int StraxInserter::ReadAndInsertData(){
  fThreadId = std::this_thread::get_id();
  fActive = fRunning = true;
  fBufferLength = 0;
  std::chrono::microseconds sleep_time(10);
  int counter = 0;
  if (fOptions->GetString("buffer_type", "dual") == "dual") {
    while(fActive == true){
      std::list<data_packet*> b;
      if (fDataSource->GetData(&b)) {
        fBufferLength = b.size();
        fBufferCounter[int(b.size())]++;
        for (auto& dp_ : b) {
          ProcessDatapacket(dp_);
          fBufferLength--;
          dp_ = nullptr;
          if (fForceQuit) break;
        }
        if (fForceQuit) for (auto& dp_ : b) if (dp_ != nullptr) delete dp_;
        b.clear();
        WriteOutFiles();
      } else {
        std::this_thread::sleep_for(sleep_time);
      }
      if (((++counter) % 1000 == 0) && (fFragments.size() > 0)) {
        std::stringstream msg;
        msg << "Current chunks " << std::hex<<fThreadId<<std::dec;
        for (auto& it : fFragments) msg << ' ' << it.first;
        fLog->Entry(MongoLog::Local, msg.str());
      }
    }
  } else {
    data_packet* dp;
    while (fActive == true) {
      if (fDataSource->GetData(dp)) {
        fBufferLength = 1;
        fBufferCounter[1]++;
        ProcessDatapacket(dp);
        fBufferLength = 0;
        WriteOutFiles();
      } else {
        std::this_thread::sleep_for(sleep_time);
      }
    }
  }
  if (fBytesProcessed > 0)
    WriteOutFiles(true);
  fRunning = false;
  return 0;
}

// Can tune here as needed, these are defaults from the LZ4 examples
static const LZ4F_preferences_t kPrefs = {
  { LZ4F_max256KB, LZ4F_blockLinked, LZ4F_noContentChecksum, LZ4F_frame, 0, { 0, 0 } },
    0,   /* compression level; 0 == default */
    0,   /* autoflush */
    { 0, 0, 0 },  /* reserved, must be set to 0 */
};

void StraxInserter::WriteOutFiles(bool end){
  // Write the contents of fFragments to compressed files
  system_clock::time_point comp_start, comp_end;
  std::vector<std::string> idx_to_clear;
  int max_chunk = -1;
  for (auto& iter : fFragments) max_chunk = std::max(max_chunk, std::stoi(iter.first));
  int write_lte = max_chunk - fBufferNumChunks;
  for (auto& iter : fFragments) {
    if (iter.first == "")
        continue; // not sure why, but this sometimes happens during bad shutdowns
    std::string chunk_index = iter.first;
    if (std::stoi(chunk_index) > write_lte && !end) continue;
    fLog->Entry(MongoLog::Local, "Thread %lx max %i current %i buffer %i write_lte %i",
        fThreadId, max_chunk, std::stoi(chunk_index), fBufferNumChunks, write_lte);

    comp_start = system_clock::now();
    if(!fs::exists(GetDirectoryPath(chunk_index, true)))
      fs::create_directory(GetDirectoryPath(chunk_index, true));

    size_t uncompressed_size = iter.second->size();

    // Compress it
    char *out_buffer = NULL;
    int wsize = 0;
    if(fCompressor == "blosc"){
      out_buffer = new char[uncompressed_size+BLOSC_MAX_OVERHEAD];
      wsize = blosc_compress_ctx(5, 1, sizeof(char), uncompressed_size,  iter.second->data(),
				   out_buffer, uncompressed_size+BLOSC_MAX_OVERHEAD, "lz4", 0, 2);
    }
    else{
      // Note: the current package repo version for Ubuntu 18.04 (Oct 2019) is 1.7.1, which is
      // so old it is not tracked on the lz4 github. The API for frame compression has changed
      // just slightly in the meantime. So if you update and it breaks you'll have to tune at least
      // the LZ4F_preferences_t object to the new format.
      size_t max_compressed_size = LZ4F_compressFrameBound(uncompressed_size, &kPrefs);
      out_buffer = new char[max_compressed_size];
      wsize = LZ4F_compressFrame(out_buffer, max_compressed_size,
				 iter.second->data(), uncompressed_size, &kPrefs);
    }
    delete iter.second;
    iter.second = nullptr;
    fFragmentSize -= uncompressed_size;
    idx_to_clear.push_back(chunk_index);

    std::ofstream writefile(GetFilePath(chunk_index, true), std::ios::binary);
    writefile.write(out_buffer, wsize);
    delete[] out_buffer;
    writefile.close();

    // shenanigans or skulduggery?
    /*if(fs::exists(GetFilePath(chunk_index, false))) {
      fLog->Entry(MongoLog::Warning, "Chunk %s already exists????", chunk_index.c_str());
    }*/

    // Move this chunk from *_TEMP to the same path without TEMP
    if(!fs::exists(GetDirectoryPath(chunk_index, false)))
      fs::create_directory(GetDirectoryPath(chunk_index, false));
    fs::rename(GetFilePath(chunk_index, true),
	       GetFilePath(chunk_index, false));
    comp_end = system_clock::now();
    fCompTime += duration_cast<microseconds>(comp_end-comp_start);

    CreateMissing(std::stoi(iter.first));
  } // End for through fragments
  // clear now because c++ sometimes overruns its buffers
  for (auto s : idx_to_clear) {
    if (fFragments.count(s) != 0) fFragments.erase(s);
  }

  if(end){
    for (auto& p : fFragments)
        if (p.second != nullptr) delete p.second;
    fFragments.clear();
    fFragmentSize = 0;
    fs::path write_path(fOutputPath);
    std::string filename = fHostname;
    write_path /= "THE_END";
    if(!fs::exists(write_path)){
      fLog->Entry(MongoLog::Local,"Creating END directory at %s",write_path.c_str());
      try{
        fs::create_directory(write_path);
      }
      catch(...){};
    }
    std::stringstream ss;
    ss<<std::this_thread::get_id();
    write_path /= fHostname + "_" + ss.str();
    std::ofstream outfile;
    outfile.open(write_path, std::ios::out);
    outfile<<"...my only friend";
    outfile.close();
  }

}

std::string StraxInserter::GetStringFormat(int id){
  std::string chunk_index = std::to_string(id);
  while(chunk_index.size() < fChunkNameLength)
    chunk_index.insert(0, "0");
  return chunk_index;
}

fs::path StraxInserter::GetDirectoryPath(std::string id, bool temp){
  fs::path write_path(fOutputPath);
  write_path /= id;
  if(temp)
    write_path+="_temp";
  return write_path;
}

fs::path StraxInserter::GetFilePath(std::string id, bool temp){
  fs::path write_path = GetDirectoryPath(id, temp);
  std::string filename = fHostname;
  std::stringstream ss;
  ss<<std::this_thread::get_id();
  filename += "_";
  filename += ss.str();
  write_path /= filename;
  return write_path;
}

void StraxInserter::CreateMissing(u_int32_t back_from_id){

  for(unsigned int x=fMissingVerified; x<back_from_id; x++){
    std::string chunk_index = GetStringFormat(x);
    std::string chunk_index_pre = chunk_index+"_pre";
    std::string chunk_index_post = chunk_index+"_post";
    if(!fs::exists(GetFilePath(chunk_index, false))){
      if(!fs::exists(GetDirectoryPath(chunk_index, false)))
	fs::create_directory(GetDirectoryPath(chunk_index, false));
      std::ofstream o;
      o.open(GetFilePath(chunk_index, false));
      o.close();
    }
    if(x!=0 && !fs::exists(GetFilePath(chunk_index_pre, false))){
      if(!fs::exists(GetDirectoryPath(chunk_index_pre, false)))
	fs::create_directory(GetDirectoryPath(chunk_index_pre, false));
      std::ofstream o;
      o.open(GetFilePath(chunk_index_pre, false));
      o.close();
    }
    if(!fs::exists(GetFilePath(chunk_index_post, false))){
      if(!fs::exists(GetDirectoryPath(chunk_index_post, false)))
	fs::create_directory(GetDirectoryPath(chunk_index_post, false));
      std::ofstream o;
      o.open(GetFilePath(chunk_index_post, false));
      o.close();
    }
  }
  fMissingVerified = back_from_id;
}


data_packet::data_packet() {
  buff = nullptr;
  size = 0;
  clock_counter = 0;
  header_time = 0;
  bid = 0;
}

data_packet::~data_packet() {
  if (buff != nullptr) delete[] buff;
  buff = nullptr;
  size = clock_counter = header_time = bid = 0;
  //vBLT.clear();
}
