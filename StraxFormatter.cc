#include "StraxFormatter.hh"
#include "DAQController.hh"
#include "MongoLog.hh"
#include "Options.hh"
#include "ThreadPool.hh"
#include <thread>
#include <cstring>
#include <cstdarg>
#include <numeric>
#include <sstream>
#include <list>
#include <bitset>
#include <iomanip>
#include <ctime>
#include <cmath>

using namespace std::chrono;
const int event_header_words = 4, max_channels = 16;

double timespec_subtract(struct timespec& a, struct timespec& b) {
  return (a.tv_sec - b.tv_sec)*1e6 + (a.tv_nsec - b.tv_nsec)/1e3;
}

StraxFormatter::StraxFormatter(){
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
  fProcTimeDP = fProcTimeEv = fProcTimeCh = fCompTime = 0.;
}

StraxFormatter::~StraxFormatter(){
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
  long total_dps = std::accumulate(fBufferCounter.begin(), fBufferCounter.end(), 0L,
      [&](long tot, auto& p){return std::move(tot) + p.second;});
  fLog->Entry(MongoLog::Local, "Thread %lx got events %.1f%% of the time",
      fThreadId, (total_dps-fBufferCounter[0]+0.0)/total_dps*100.);
  std::map<std::string, long> counters {
    {"bytes", fBytesProcessed},
    {"fragments", fFragmentsProcessed},
    {"events", fEventsProcessed},
    {"data_packets", total_dps - fBufferCounter[0]}};
  fOptions->SaveBenchmarks(counters, fBufferCounter,
      fProcTimeDP, fProcTimeEv, fProcTimeCh, fCompTime);
}

int StraxFormatter::Initialize(Options *options, MongoLog *log, DAQController *dataSource,
			      std::string hostname){
  fOptions = options;
  fFragmentBytes = fOptions->GetInt("strax_fragment_payload_bytes", 110*2);

  fDataSource = dataSource;
  dataSource->GetDataFormat(fFmt);
  fLog = log;
  fErrorBit = false;

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
    fLog->Entry(MongoLog::Error, "StraxInserter::Initialize tried to create output directory but failed. Check that you have permission to write here.");
    return -1;
  }
  fProcTimeDP = fProcTimeEv = fProcTimeCh = fCompTime = microseconds(0);

  return 0;
}

void StraxFormatter::Close(std::map<int,int>& ret){
  fActive = false;
  const std::lock_guard<std::mutex> lg(fFC_mutex);
  for (auto& iter : fFailCounter) ret[iter.first] += iter.second;
}

void StraxFormatter::GetDataPerChan(std::map<int, int>& ret) {
  if (!fActive) return;
  fDPC_mutex.lock();
  for (auto& pair : fDataPerChan) {
    ret[pair.first] += pair.second;
    pair.second = 0;
  }
  fDPC_mutex.unlock();
  return;
}

void StraxFormatter::GenerateArtificialDeadtime(int64_t timestamp, int16_t bid, uint32_t et, int ro) {
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
  AddFragmentToBuffer(fragment, timestamp, et, ro);
}

void StraxFormatter::ProcessDatapacket(std::string&& str, const int& bid, const int& words, const uint32_t& clock_counter, const uint32_t& header_time){

  struct timespec dp_start, dp_end, ev_start, ev_end;

  // Take a buffer and break it up into one document per channel

  u_int32_t *buff = dp->buff;
  u_int32_t idx = 0;
  unsigned total_words = dp->size/sizeof(u_int32_t);
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &dp_start);
  while(idx < total_words && fForceQuit == false){

    if(buff[idx]>>28 == 0xA){ // 0xA indicates header at those bits
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ev_start);
      idx += ProcessEvent(buff+idx, total_words-idx, dp->clock_counter, dp->header_time, dp->bid);
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ev_end);
      fProcTimeEv += timespec_subtract(ev_end, ev_start);
    } else
      idx++;
  }
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &dp_end);
  fProcTimeDP += timespec_subtract(dp_end, dp_start);
  fBytesProcessed += dp->size;
  delete dp;
}

uint32_t StraxFormatter::ProcessEvent(const uint32_t* buff, const unsigned& total_words,
    const long clock_counter&, const uint32_t& header_time, const int& bid) {
  // buff = start of event, total_words = valid words remaining in total buffer

  struct timespec ch_start, ch_end;
  std::map<std::string, int> fmt = fFmt[bid];

  u_int32_t words_in_event = std::min(buff[0]&0xFFFFFFF, total_words);
  if (words_in_event < (buff[0]&0xFFFFFFF)) {
    fLog->Entry(MongoLog::Local, "Board %i garbled event header: %x/%x",
        bid, buff[0]&0xFFFFFFF, total_words);
  }

  u_int32_t channel_mask = (buff[1]&0xFF);
  if (fmt["channel_mask_msb_idx"] != -1) channel_mask |= ( ((buff[2]>>24)&0xFF)<<8);

  u_int32_t event_time = buff[3]&0x7FFFFFFF;
  fEventsProcessed++;

  if(buff[1]&0x4000000){ // board fail
    const std::lock_guard<std::mutex> lg(fFC_mutex);
    GenerateArtificialDeadtime(((clock_counter<<31) + header_time)*fmt["ns_per_clock"], bid,
        event_time, clock_counter);
    fDataSource->CheckError(bid);
    fFailCounter[bid]++;
    return event_header_words;
  }

  unsigned idx = event_header_words;
  int ret;

  for(unsigned ch=0; ch<max_channels; ch++){
    if (channel_mask & (1<<ch)) {
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ch_start);
      ret = ProcessChannel(buff+idx, words_in_event, bid, ch, header_time, event_time,
        clock_counter, channel_mask);
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ch_end);
      fProcTimeCh += timespec_subtract(ch_end, ch_start);
      if (ret == -1)
        break;
      idx += ret;
    }
  }
  return idx;
}

int StraxFormatter::ProcessChannel(uint32_t* buff, unsigned words_in_event, int bid, int channel,
    uint32_t header_time, uint32_t event_time, long clock_counter, int channel_mask) {
  // buff points to the first word of the channel's data

  // These defaults are valid for 'default' firmware where all channels are the same size
  int channels_in_event = std::bitset<max_channels>(channel_mask).count();
  u_int32_t channel_words = (words_in_event-event_header_words) / channels_in_event;
  long channel_time = event_time;
  long channel_timeMSB = clock_counter<<31;
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
    channel_time = buff[1] & (fmt["channel_time_msb_idx"] == -1 ? 0x7FFFFFFF : 0xFFFFFFFF);

    if (fmt["channel_time_msb_idx"] == 2) {
      channel_time = buff[1];
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
      fLog->Entry(MongoLog::Local, "Board %i has CAEN'd itself (%lx)", bid, Time64);
      GenerateArtificialDeadtime(Time64, bid, event_time, clock_counter);
      return -1;
    }
  }

  u_int16_t *payload = reinterpret_cast<u_int16_t*>(buff+fmt["channel_header_words"]);
  u_int32_t samples_in_pulse = (channel_words-fmt["channel_header_words"])<<1;
  u_int16_t sw = fmt["ns_per_sample"];
  int samples_per_fragment = fFragmentBytes>>1;
  int16_t cl = fOptions->GetChannel(bid, channel);
  // Failing to discern which channel we're getting data from seems serious enough to throw
  if(cl==-1)
    throw std::runtime_error("Failed to parse channel map. I'm gonna just kms now.");

  int num_frags = std::ceil(1.*samples_in_pulse/samples_per_fragment);
  for (uint16_t frag_i = 0; frag_i < num_frags; frag_i++) {
    std::string fragment;
    fragment.reserve(fFragmentBytes + fStraxHeaderSize);

    // How long is this fragment?
    u_int32_t samples_this_fragment = samples_per_fragment;
    if (frag_i == num_frags-1)
      samples_this_fragment = samples_in_pulse - frag_i*samples_per_fragment;
    fFragmentsProcessed++;

    u_int64_t time_this_fragment = Time64 + samples_per_fragment*sw*frag_i;
    fragment.append((char*)&time_this_fragment, sizeof(time_this_fragment));
    fragment.append((char*)&samples_this_fragment, sizeof(samples_this_fragment));
    fragment.append((char*)&sw, sizeof(sw));
    fragment.append((char*)&cl, sizeof(cl));
    fragment.append((char*)&samples_in_pulse, sizeof(samples_in_pulse));
    fragment.append((char*)&frag_i, sizeof(frag_i));
    fragment.append((char*)&baseline_ch, sizeof(baseline_ch));

    // Copy the raw buffer
    fragment.append((char*)(payload + frag_i*samples_per_fragment), samples_this_fragment*2);
    uint16_t zero_filler = 0;
    while((int)fragment.size()<fFragmentBytes+fStraxHeaderSize)
      fragment.append((char*)&zero_filler, 2);

    AddFragmentToBuffer(fragment, time_this_fragment, event_time, clock_counter);
  } // loop over frag_i
  {
    const std::lock_guard<std::mutex> lg(fDPC_mutex);
    fDataPerChan[cl] += samples_in_pulse<<1;
  }
  return channel_words;
}

void StraxFormatter::AddFragmentToBuffer(std::string& fragment, int64_t timestamp, uint32_t ts, int rollovers) {
  // Get the CHUNK and decide if this event also goes into a PRE/POST file
  int chunk_id = timestamp/fFullChunkLength;
  bool nextpre = (chunk_id+1)* fFullChunkLength - timestamp <= fChunkOverlap;
  // Minor mess to maintain the same width of file names and do the pre/post stuff
  // If not in pre/post
  std::string chunk_index = GetStringFormat(chunk_id);
  int min_chunk(0), max_chunk(1);
  if (fFragments.size() > 0) {
    const auto [min_chunk_, max_chunk_] = std::minmax_element(fFragments.begin(), fFragments.end(), 
      [&](auto& l, auto& r) {return std::stoi(l.first) < std::stoi(r.first);});
    min_chunk = std::stoi((*min_chunk_).first);
    max_chunk = std::stoi((*max_chunk_).first);
  }

  if (min_chunk - chunk_id > fWarnIfChunkOlderThan) {
    const short* channel = (const short*)(fragment.data()+14);
    fLog->Entry(MongoLog::Warning,
        "Thread %lx got data from ch %i that's in chunk %i instead of %i/%i (ts %lx), it might get lost (ts %lx ro %i)",
        fThreadId, *channel, chunk_id, min_chunk, max_chunk, timestamp, ts, rollovers);
  } else if (chunk_id - max_chunk > 2) {
    fLog->Entry(MongoLog::Message, "Thread %lx skipped %i chunk(s)",
        fThreadId, chunk_id - max_chunk - 1);
  }

  fFragmentSize += fragment.size();

  if(!nextpre){
    if(fFragments.count(chunk_index) == 0){
      fFragments[chunk_index] = new std::string();
    }
    fFragments[chunk_index]->append(fragment);
  } else {
    std::string nextchunk_index = GetStringFormat(chunk_id+1);

    if(fFragments.count(nextchunk_index+"_pre") == 0){
      fFragments[nextchunk_index+"_pre"] = new std::string();
    }
    fFragments[nextchunk_index+"_pre"]->append(fragment);

    if(fFragments.count(chunk_index+"_post") == 0){
      fFragments[chunk_index+"_post"] = new std::string();
    }
    fFragments[chunk_index+"_post"]->append(fragment);
  }
}

int StraxFormatter::ReadAndInsertData(){
  fThreadId = std::this_thread::get_id();
  fActive = fRunning = true;
  fBufferLength = 0;
  std::chrono::microseconds sleep_time(10);
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
        fBufferCounter[0]++;
        std::this_thread::sleep_for(sleep_time);
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

