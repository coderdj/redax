#include "V1730.hh"
#include "MongoLog.hh"
#include "Options.hh"

V1730::V1730(std::shared_ptr<ThreadPool>& tp, std::shared_ptr<Processor>& next,
    std::shared_ptr<Options>& opts, std::shared_ptr<MongoLog>& log)
  :V1724(tp, next, opts, log){
  fNChannels = 16;
  fArtificialDeadtimeChannel = 792;
  fClockCycle = 2;
  fSampleWidth = 2;
}

V1730::~V1730(){}

std::tuple<int, int, bool, uint32_t> V1724::UnpackEventHeader(std::u32string_view sv) {
  // returns {words this event, channel mask, board fail, header timestamp}
  return {sv[0]&0xFFFFFFF,
         (sv[1]&0xFF) | ((sv[2]>>16)&0xFF00),
          sv[1]&0x4000000,
          sv[3]&0x7FFFFFFF};
}

std::tuple<int64_t, int, uint16_t, std::u32string_view>
V1724::UnpackChannelHeader(std::u32string_view sv, long, uint32_t, uint32_t, int, int) {
  // returns {timestamp (ns), words this channel, baseline, waveform}
  int words = sv[0]&0x7FFFFF;
  return {(long(sv[1]) | (long(sv[2]&0xFFFF)<<32))*fClockCycle,
          words,
          (sv[2]>>16)&0x3FFF,
          sv.substr(3, words-3)};
}
