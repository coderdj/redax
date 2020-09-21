#include "V1724.hh"
#include <numeric>
#include <algorithm>
#include <bitset>
#include <cmath>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include "MongoLog.hh"
#include "Options.hh"
#include <CAENVMElib.h>
#include <sstream>
#include <utility>


V1724::V1724(std::shared_ptr<ThreadPool>& tp, std::shared_ptr<Processor>& next, std::shared_ptr<Options>& opts, std::shared_ptr<MongoLog>& log) : 
  Processor(tp, next, opts, log), fDPoverhead(3), fCHoverhead(5) {
  fBoardHandle=fLink=fCrate=fBID=-1;
  fBaseAddress=0;

  fAqCtrlRegister = 0x8100;
  fAqStatusRegister = 0x8104;
  fSwTrigRegister = 0x8108;
  fResetRegister = 0xEF24;
  fChStatusRegister = 0x1088;
  fChDACRegister = 0x1098;
  fNChannels = 8;
  fChTrigRegister = 0x1060;
  fSNRegisterMSB = 0xF080;
  fSNRegisterLSB = 0xF084;
  fBoardFailStatRegister = 0x8178;
  fReadoutStatusRegister = 0xEF04;
  fBoardErrRegister = 0xEF00;

  fArtificialDeadtimeChannel = 790;
  fClockCycle = 10; // ns
  fSampleWidth = 10; // ns
  fRolloverCounter = 0;
  fLastClock = 0;
  // there's a more elegant way to do this, but I'm not going to write it
  fClockPeriod = std::chrono::nanoseconds((1l<<31)*fClockCycle);

  BLT_SIZE=512*1024; // one channel's memory

  fBLTSafety = 1.4;
  fMissed = 0;
  fFailures = 0;
  fCheckFail = false;
}

V1724::~V1724(){
  End();
  if (fBLTCounter.empty()) return;
  std::stringstream msg;
  msg << "BLT report for board " << fBID << " (BLT " << BLT_SIZE << ")";
  for (auto p : fBLTCounter) msg << " | " << p.first << " " << int(std::log2(p.second));
  fLog->Entry(MongoLog::Local, msg.str());
}

int V1724::SINStart(){
  fLastClockTime = std::chrono::high_resolution_clock::now();
  return WriteRegister(fAqCtrlRegister,0x105);
}
int V1724::SoftwareStart(){
  fLastClockTime = std::chrono::high_resolution_clock::now();
  return WriteRegister(fAqCtrlRegister, 0x104);
}
int V1724::AcquisitionStop(bool){
  return WriteRegister(fAqCtrlRegister, 0x100);
}
int V1724::SWTrigger(){
  return WriteRegister(fSwTrigRegister, 0x1);
}
bool V1724::EnsureReady(int ntries, int tsleep){
  return MonitorRegister(fAqStatusRegister, 0x100, ntries, tsleep, 0x1);
}
bool V1724::EnsureStarted(int ntries, int tsleep){
  return MonitorRegister(fAqStatusRegister, 0x4, ntries, tsleep, 0x1);
}
bool V1724::EnsureStopped(int ntries, int tsleep){
  return MonitorRegister(fAqStatusRegister, 0x4, ntries, tsleep, 0x0);
}
u_int32_t V1724::GetAcquisitionStatus(){
  return ReadRegister(fAqStatusRegister);
}

int V1724::CheckErrors(){
  auto pll = ReadRegister(fBoardFailStatRegister);
  auto ros = ReadRegister(fReadoutStatusRegister);
  unsigned ERR = 0xFFFFFFFF;
  if ((pll == ERR) || (ros == ERR)) return -1;
  int ret = 0;
  if (pll & (1 << 4)) ret |= 0x1;
  if (ros & (1 << 2)) ret |= 0x2;
  return ret;
}

int V1724::Init(int link, int crate, int bid, unsigned int address){
  int a = CAENVME_Init(cvV2718, link, crate, &fBoardHandle);
  if(a != cvSuccess){
    fLog->Entry(MongoLog::Warning, "Board %i failed to init, error %i handle %i link %i bdnum %i",
            bid, a, fBoardHandle, link, crate);
    fBoardHandle = -1;
    return -1;
  }
  fLog->Entry(MongoLog::Debug, "Board %i initialized with handle %i (link/crate)(%i/%i)",
	      bid, fBoardHandle, link, crate);  

  fLink = link;
  fCrate = crate;
  fBID = bid;
  fBaseAddress=address;
  uint32_t word(0);
  int my_bid(0);

  fBLTSafety = fOptions->GetDouble("blt_safety_factor", 1.5);
  BLT_SIZE = fOptions->GetInt("blt_size", 512*1024);

  if (Reset()) {
    fLog->Entry(MongoLog::Error, "Board %i unable to pre-load registers", fBID);
    return -1;
  } else {
    fLog->Entry(MongoLog::Local, "Board %i reset", fBID);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (fOptions->GetInt("do_sn_check", 0) != 0) {
    if ((word = ReadRegister(fSNRegisterLSB)) == 0xFFFFFFFF) {
      fLog->Entry(MongoLog::Error, "Board %i couldn't read its SN lsb", fBID);
      return -1;
    }
    my_bid |= word&0xFF;
    if ((word = ReadRegister(fSNRegisterMSB)) == 0xFFFFFFFF) {
      fLog->Entry(MongoLog::Error, "Board %i couldn't read its SN msb", fBID);
      return -1;
    }
    my_bid |= ((word&0xFF)<<8);
    if (my_bid != fBID) {
      fLog->Entry(MongoLog::Local, "Link %i crate %i should be SN %i but is actually %i",
        link, crate, fBID, my_bid);
    }
  }
  return 0;
}

int V1724::Reset() {
  int ret = WriteRegister(fResetRegister, 0x1);
  ret += WriteRegister(fBoardErrRegister, 0x30);
  return ret;
}

uint32_t V1724::GetHeaderTime(char32_t* buff, int size){
  int idx = 0;
  while(idx < size){
    if(buff[idx]>>28==0xA){
      return buff[idx+3]&0x7FFFFFFF;
    }
    idx++;
  }
  return 0xFFFFFFFF;
}

int V1724::GetClockCounter(uint32_t timestamp){
  // The V1724 has a 31-bit on board clock counter that counts 10ns samples.
  // So it will reset every 21 seconds. We need to count the resets or we
  // can't run longer than that. We can employ some clever logic
  // and real-time time differences to handle clock rollovers and catch any
  // that we happen to miss the usual way

  auto now = std::chrono::high_resolution_clock::now();
  std::chrono::nanoseconds dt = now - fLastClockTime;
  fLastClockTime += dt; // no operator=

  int n_missed = dt / fClockPeriod;
  if (n_missed > 0) {
    fLog->Entry(MongoLog::Message, "Board %i missed %i rollovers", fBID, n_missed);
    fRolloverCounter += n_missed;
  }

  if (timestamp < fLastClock) {
    // actually rolled over
    fRolloverCounter++;
    fLog->Entry(MongoLog::Local, "Board %i rollover %i (%x/%x)",
            fBID, fRolloverCounter, fLastClock, timestamp);
  } else {
    // not a rollover
  }
  fLastClock = timestamp;
  return fRolloverCounter;
}

int V1724::WriteRegister(unsigned int reg, unsigned int value){
  uint32_t write=value;
  int ret = 0;
  if((ret = CAENVME_WriteCycle(fBoardHandle, fBaseAddress+reg,
			&write,cvA32_U_DATA,cvD32)) != cvSuccess){
    fLog->Entry(MongoLog::Warning,
		"Board %i write returned %i (ret), reg 0x%04x, value 0x%08x",
		fBID, ret, reg, value);
    return -1;
  }
  return 0;
}

unsigned int V1724::ReadRegister(unsigned int reg){
  unsigned int temp;
  int ret = -100;
  if((ret = CAENVME_ReadCycle(fBoardHandle, fBaseAddress+reg, &temp,
			      cvA32_U_DATA, cvD32)) != cvSuccess){
    fLog->Entry(MongoLog::Warning,
		"Board %i read returned: %i (ret) 0x%08x (val) for reg 0x%04x",
		fBID, ret, temp, reg);
    return 0xFFFFFFFF;
  }
  return temp;
}

int V1724::Read(std::u32string* outptr){
  if ((GetAcquisitionStatus() & 0x8) == 0) return 0;
  // Initialize
  int blt_words=0, nb=0, ret=-5;
  std::vector<std::pair<char32_t*, int>> xfer_buffers;

  int count = 0;
  int alloc_size = BLT_SIZE*fBLTSafety/sizeof(char32_t);
  char32_t* thisBLT = nullptr;
  do{
    // Reserve space for this block transfer
    thisBLT = new char32_t[alloc_size];

    ret = CAENVME_FIFOBLTReadCycle(fBoardHandle, fBaseAddress, thisBLT,
				     BLT_SIZE, cvA32_U_MBLT, cvD64, &nb);
    if( (ret != cvSuccess) && (ret != cvBusError) ){
      fLog->Entry(MongoLog::Error,
		  "Board %i read error after %i reads: (%i) and transferred %i bytes this read",
		  fBID, count, ret, nb);

      // Delete all reserved data and fail
      delete[] thisBLT;
      for (auto& b : xfer_buffers) delete[] b.first;
      return -1;
    }
    if (nb > BLT_SIZE) fLog->Entry(MongoLog::Message,
        "Board %i got %i more bytes than asked for (headroom %i)",
        fBID, nb-BLT_SIZE, alloc_size*sizeof(char32_t)-nb);

    count++;
    blt_words+=nb/sizeof(char32_t);
    xfer_buffers.emplace_back(thisBLT, nb/sizeof(char32_t));

  }while(ret != cvBusError);

  // Now, unfortunately we need to make one copy of the data here or else our memory
  // usage explodes. We declare above a buffer of several MB, which is the maximum capacity
  // of the board in case every channel is 100% saturated (the practical largest
  // capacity is certainly smaller depending on settings). But if we just keep reserving
  // O(MB) blocks and filling 50kB with actual data, we're gonna run out of memory.
  // So here we declare the return buffer as *just* large enough to hold the actual
  // data and free up the rest of the memory reserved as buffer.
  // In tests this does not seem to impact our ability to read out the V1724 at the
  // maximum bandwidth of the link.
  if(blt_words>0){
    // data packet header:
    // 1 word, task code
    // 1 word, header time
    // 1 word, clock counter
    alloc_size = blt_words + fDPoverhead;
    std::u32string dp;
    dp.reserve(alloc_size);
    dp += ThreadPool::TaskCode::UnpackDatapacket;
    uint32_t word = 0;
    word = GetHeaderTime(xfer_buffers.front().first, xfer_buffers.front().second);
    int clock_counter = GetClockCounter(word);
    dp += word;
    dp += clock_counter;
    int words_copied = 0;
    for (auto& xfer : xfer_buffers) {
      dp.append(xfer.first, xfer.second);
      words_copied += xfer.second;
    }
    fBLTCounter[count]++;
    if (words_copied != blt_words) fLog->Entry(MongoLog::Message,
        "Board %i funny buffer accumulation: %i/%i from %i BLTs",
        fBID, words_copied, blt_words, count);
    if (outptr != nullptr)
      *outptr = dp.substr(fDPoverhead);
    else
      fTP->AddTask(this, std::move(dp));
  }
  for (auto& b : xfer_buffers) delete[] b.first;
  return blt_words;
}

int V1724::LoadDAC(std::vector<uint16_t> &dac_values){
  // Loads DAC values into registers
  for(unsigned int x=0; x<fNChannels; x++){
    if(WriteRegister((fChDACRegister)+(0x100*x), dac_values[x])!=0){
      fLog->Entry(MongoLog::Error, "Board %i failed writing DAC 0x%04x in channel %i",
		  fBID, dac_values[x], x);
      return -1;
    }

  }
  return 0;
}

int V1724::SetThresholds(std::vector<u_int16_t> vals) {
  int ret = 0;
  for (unsigned ch = 0; ch < fNChannels; ch++)
    ret += WriteRegister(fChTrigRegister + 0x100*ch, vals[ch]);
  return ret;
}

void V1724::End(){
  if(fBoardHandle>=0)
    CAENVME_End(fBoardHandle);
  fBoardHandle=fLink=fCrate=-1;
  fBaseAddress=0;
  if (fMissed > 0) fLog->Entry(MongoLog::Local, "Board %i missed %i events", fBID, fMissed.load());
  return;
}

void V1724::ClampDACValues(std::vector<u_int16_t> &dac_values,
                  std::map<std::string, std::vector<double>> &cal_values) {
  u_int16_t min_dac, max_dac(0xffff);
  for (unsigned ch = 0; ch < fNChannels; ch++) {
    if (cal_values["yint"][ch] > 0x3fff) {
      min_dac = (0x3fff - cal_values["yint"][ch])/cal_values["slope"][ch];
    } else {
      min_dac = 0;
    }
    dac_values[ch] = std::clamp(dac_values[ch], min_dac, max_dac);
    if ((dac_values[ch] == min_dac) || (dac_values[ch] == max_dac)) {
      fLog->Entry(MongoLog::Local, "Board %i channel %i clamped dac to 0x%04x",
	fBID, ch, dac_values[ch]);
    }
  }
}

bool V1724::MonitorRegister(u_int32_t reg, u_int32_t mask, int ntries, int sleep, u_int32_t val){
  u_int32_t rval = 0;
  if(val == 0) rval = 0xffffffff;
  for(int counter = 0; counter < ntries; counter++){
    rval = ReadRegister(reg);
    if(rval == 0xffffffff)
      break;
    if((val == 1 && (rval&mask)) || (val == 0 && !(rval&mask)))
      return true;
    usleep(sleep);
  }
  fLog->Entry(MongoLog::Warning,"Board %i MonitorRegister failed for 0x%04x with mask 0x%04x and register value 0x%04x, wanted 0x%04x",
          fBID, reg, mask, rval,val);
  return false;
}

void V1724::Process(std::u32string_view sv) { // sv = data packet
  return DPtoChannels(sv);
}

void V1724::DPtoChannels(std::u32string_view sv) {
  uint32_t word;
  auto it = sv.begin() + fDPoverhead;
  bool bMissed = true;
  std::vector<std::u32string> channels;
  channels.reserve(fNChannels); // magic number? no clue
  uint32_t header_time = sv[1];
  long clock_counter = sv[2];
  while (it < sv.end()) {
    if ((*it)>>28 == 0xA) {
      bMissed = false;
      word = (*it)&0xFFFFFFF;
      EventToChannels(sv.substr(std::distance(sv.begin(), it), word), header_time,
          clock_counter, channels);
      it += word;
    } else {
      if (!bMissed) {
        fLog->Entry(MongoLog::Local, "Bd %i missed at %i (%i)",
                fBID, std::distance(it, sv.begin()), sv.size());
        fMissed++;
        bMissed = true;
      }
      it++;
    }
  }
  if (channels.size() == 1)
    fTP->AddTask(this, std::move(channels[0]));
  else
    fTP->AddTask(this, channels);
  return;
}

void V1724::EventToChannels(std::u32string_view sv, uint32_t header_time,
    long clock_counter, std::vector<std::u32string>& channels) {
  // channel header format:
  // 2 words timestamp
  // 1 word (ch in MSB, board id in LSB)
  // 1 word (sample width, baseline)
  const int event_header_words(4);
  auto [words, mask, fail, event_time] = UnpackEventHeader(sv);
  if (fail) {
    fFailures++;
    fCheckFail = true;
    auto s = GenerateArtificialDeadtime(((clock_counter<<31)+event_time)*fClockCycle);
    channels.push_back(std::move(s));
    return;
  }
  sv.remove_prefix(event_header_words);
  int n_channels = std::bitset<16>(mask).count();
  for (unsigned ch = 0; ch < fNChannels; ch++) {
    if (mask & (1<<ch)) {
      std::u32string channel;
      auto [timestamp, words_this_ch, baseline, wf] = UnpackChannelHeader(sv,
          clock_counter, header_time, event_time, words, n_channels);
      channel.reserve(fCHoverhead + wf.size());
      channel += ThreadPool::TaskCode::UnpackChannel;
      sv.remove_prefix(words_this_ch);

      channel.append((char32_t*)&timestamp, sizeof(timestamp)/sizeof(char32_t));
      uint32_t word = fBID;
      word |= (ch << 16);
      channel += word;
      word = baseline | (fSampleWidth << 16);
      channel += word;
      channel += wf;

      channels.emplace_back(std::move(channel));
    } // if mask
  } // for ch in channels
}

std::u32string V1724::GenerateArtificialDeadtime(int64_t ts) {
  std::u32string data;
  data.reserve(fCHoverhead + 1);
  data.append((char32_t*)&ts, sizeof(ts)/sizeof(char32_t));
  uint32_t word = fBID;
  word |= (fArtificialDeadtimeChannel << 16);
  data += word;
  word = fSampleWidth << 16;
  data += word;
  word = 0;
  data += word;
  return data;
}

std::tuple<int, int, bool, uint32_t> V1724::UnpackEventHeader(std::u32string_view sv) {
  // returns {words this event, channel mask, board fail, header timestamp}
  return {sv[0]&0xFFFFFFF, sv[1]&0xFF, sv[1]&0x4000000, sv[3]&0x7FFFFFFF};
}

std::tuple<int64_t, int, uint16_t, std::u32string_view> V1724::UnpackChannelHeader(std::u32string_view sv, long rollovers, uint32_t header_time, uint32_t, int, int) {
  // returns {timestamp (ns), words this channel, baseline, waveform}
  long ch_time = sv[1]&0x7FFFFFFF;
  int words = sv[0]&0x7FFFFF;
  // More rollover logic here, because channels are independent and the
  // processing is multithreaded. We leverage the fact that readout windows are
  // short and polled frequently compared to the rollover timescale, so there
  // will never be a large difference in timestamps in one data packet
  if (ch_time > 15e8 && header_time < 5e8 && rollovers != 0) rollovers--;
  else if (ch_time < 5e8 && header_time > 15e8) rollovers++;
  return {((rollovers<<31)+ch_time)*fClockCycle, words, 0, sv.substr(2, words-2)};
}

