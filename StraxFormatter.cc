#include "StraxFormatter.hh"
#include "MongoLog.hh"
#include "Options.hh"
#include "ThreadPool.hh"
#include "Compressor.hh"
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
const int max_channels = 16;

double timespec_subtract(struct timespec& a, struct timespec& b) {
  return (a.tv_sec - b.tv_sec)*1e6 + (a.tv_nsec - b.tv_nsec)/1e3;
}

StraxFormatter::StraxFormatter(std::shared_ptr<ThreadPool>& tp, std::shared_ptr<Processor>& next, std::shared_ptr<Options>& opts, std::shared_ptr<MongoLog>& log) : Processor(tp, next, opts, log) {
  fStraxHeaderSize=24;
  fFragmentBytes=fOptions->GetInt("strax_fragment_payload_bytes", 220);
  fSamplesPerFrag = fFragmentBytes/sizeof(uint16_t);
}

StraxFormatter::~StraxFormatter(){
}

std::map<int, int> StraxFormatter::GetDataPerChan() {
  std::map<int, int> ret;
  const std::lock_guard<std::mutex> lg(fMutex);
  for (auto& pair : fDataPerChan) {
    ret[pair.first] += pair.second;
    pair.second = 0;
  }
  return ret;
}

void StraxFormatter::Process(std::u32string_view protofrag) {
  // Transform digitizer-agnostic packets into fragments
  protofrag.remove_prefix(1);
  int64_t timestamp = *(int64_t*)protofrag.data();
  uint32_t word = protofrag[2];
  int16_t ch = word>>16, bid = word&0xFFFF;
  word = protofrag[3];
  uint16_t sw = word>>16, bl = word&0xFFFF;
  //fLog->Entry(MongoLog::Local, "SF %lx %x %x %x %x %x %x",
  //        timestamp, protofrag[2], protofrag[3], ch, bid, sw, bl);
  int16_t global_ch;
  const int samples_per_word = 2;
  if ((global_ch = fOptions->GetChannel(bid, ch)) == -1) {
    // can't parse channel map, so commit suicide
    std::string message = "Can't parse cable map, bid " + std::to_string(bid) + " " + std::to_string(ch);
    fLog->Entry(MongoLog::Fatal, message);
    throw std::runtime_error(message.c_str());
  }

  protofrag.remove_prefix(4);
  int samples_in_pulse = protofrag.size()*samples_per_word;
  int num_frags = std::ceil(1.*samples_in_pulse/fSamplesPerFrag);
  uint16_t* wf = (uint16_t*)protofrag.data();

  {
    const std::lock_guard<std::mutex> lg(fMutex);
    fDataPerChan[global_ch] += protofrag.size()*sizeof(char32_t);
  }

  for (int frag_i = 0; frag_i < num_frags; frag_i++) {
    std::string fragment;
    fragment.reserve(fFragmentBytes);
    uint32_t samples_this_fragment = fSamplesPerFrag;
    if (frag_i == num_frags-1)
      samples_this_fragment = samples_in_pulse - frag_i*fSamplesPerFrag;

    int64_t time_this_fragment = timestamp + fSamplesPerFrag*sw*frag_i;
    fragment.append((char*)&time_this_fragment, sizeof(time_this_fragment));
    fragment.append((char*)&samples_this_fragment, sizeof(samples_this_fragment));
    fragment.append((char*)&sw, sizeof(sw));
    fragment.append((char*)&global_ch, sizeof(global_ch));
    fragment.append((char*)&samples_in_pulse, sizeof(samples_in_pulse));
    fragment.append((char*)&frag_i, sizeof(frag_i));
    fragment.append((char*)&bl, sizeof(bl));

    // Copy the raw buffer
    fragment.append((char*)(wf + frag_i*fSamplesPerFrag), samples_this_fragment*2);
    uint16_t zero_filler = 0;
    while((int)fragment.size()<fFragmentBytes+fStraxHeaderSize)
      fragment.append((char*)&zero_filler, sizeof(zero_filler));

    static_cast<Compressor*>(fNext.get())->AddFragmentToBuffer(std::move(fragment));
  }
  return;
}

