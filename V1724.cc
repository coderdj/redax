#include "V1724.hh"
#include <numeric>
#include <array>
#include <algorithm>
#include <bitset>
#include <cmath>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include "MongoLog.hh"
#include "Options.hh"
#include <CAENVMElib.h>


V1724::V1724(MongoLog  *log, Options *options){
  fOptions = options;
  fBoardHandle=fLink=fCrate=fBID=-1;
  fBaseAddress=0;
  fLog = log;

  fAqCtrlRegister = 0x8100;
  fAqStatusRegister = 0x8104;
  fSwTrigRegister = 0x8108;
  fResetRegister = 0xEF24;
  fChStatusRegister = 0x1088;
  fChDACRegister = 0x1098;
  fNChannels = 8;
  fChTrigRegister = 0x1060;
  fSNRegisterMSB = 0xF080;
  fSNRegisterLSB = 0xF040;

  DataFormatDefinition = {
    {"channel_mask_msb_idx", -1},
    {"channel_mask_msb_mask", -1},
    {"channel_header_words", 2},
    {"ns_per_sample", 10},
    {"ns_per_clk", 10},
    // Channel indices are given relative to start of channel
    // i.e. the channel size is at index '0'
    {"channel_time_msb_idx", -1},
    {"channel_time_msb_mask", -1},

  };
}

V1724::~V1724(){
  End();
}

int V1724::SINStart(){
  return WriteRegister(fAqCtrlRegister,0x105);
}
int V1724::SoftwareStart(){
  return WriteRegister(fAqCtrlRegister, 0x104);
}
int V1724::AcquisitionStop(){
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


int V1724::Init(int link, int crate, int bid, unsigned int address){
  int a = CAENVME_Init(cvV2718, link, crate, &fBoardHandle);
  if(a != cvSuccess){
    fLog->Entry(MongoLog::Warning, "Board %i failed to init, error %i handle %i link %i bdnum %i",
            fBID, a, fBoardHandle, link, crate);
    fBoardHandle = -1;
    return -1;
  }
  fLog->Entry(MongoLog::Debug, "Board %i initialized with handle %i (link/crate)(%i/%i)",
	      bid, fBoardHandle, link, crate);  

  fLink = link;
  fCrate = crate;
  fBID = bid;
  fBaseAddress=address;
  clock_counter = 0;
  last_time = 0;
  seen_over_15 = false;
  seen_under_5 = true; // starts run as true
  u_int32_t word(0);
  int my_bid(0);
  if (CAENVME_ReadCycle(fBoardHandle, fSNRegisterLSB, &word, cvA32_U_DATA, cvD32) != cvSuccess) {
    fLog->Entry(MongoLog::Error, "Board %i couldn't read its SN lsb", fBoardHandle);
    return -1;
  }
  my_bid |= word&0xFF;
  if (CAENVME_ReadCycle(fBoardHandle, fSNRegisterMSB, &word, cvA32_U_DATA, cvD32) != cvSuccess) {
    fLog->Entry(MongoLog::Error, "Board %i couldn't read its SN msb", fBoardHandle);
    return -1;
  }
  my_bid |= (word&0xFF)<<8;
  if (my_bid != fBID) {
    fLog->Entry(MongoLog::Message, "Link %i crate %i should be SN %i but is actually %i",
        link, crate, fBID, my_bid);
    return 0;
  }
  return 0;
}

u_int32_t V1724::GetHeaderTime(u_int32_t *buff, u_int32_t size){
  u_int32_t idx = 0;
  while(idx < size/sizeof(u_int32_t)){
    if(buff[idx]>>28==0xA)
      return buff[idx+3]&0x7FFFFFFF;
    idx++;
  }
  return 0xFFFFFFFF;
}

int V1724::GetClockCounter(u_int32_t timestamp){
  // The V1724 has a 31-bit on board clock counter that counts 10ns samples.
  // So it will reset every 21 seconds. We need to count the resets or we
  // can't run longer than that. But it's not as simple as incementing a
  // counter every time a timestamp is less than the previous one because
  // we're multi-threaded and channels are quasi-independent. So we need
  // this fancy logic here.

  //Seen under 5, true first time you see something under 5. False first time you
  // see something under 15 but >5
  // Seen over 15, true first time you se something >15 if under 5=false. False first
  // time you see something under 5

  // First, is this number greater than the previous?
  if(timestamp > last_time){

    // Case 1. This is over 15s but seen_under_5 is true. Give 1 back
    if(timestamp >= 15e8 && seen_under_5 && clock_counter != 0)
      return clock_counter-1;

    // Case 2. This is over 5s and seen_under_5 is true.
    else if(timestamp >= 5e8 && timestamp < 15e8 && seen_under_5){
      seen_under_5 = false;
      last_time = timestamp;
      return clock_counter;
    }

    // Case 3. This is over 15s and seen_under_5 is false
    else if(timestamp >= 15e8 && !seen_under_5){
      seen_over_15 = true;
      last_time = timestamp;
      return clock_counter;
    }

    // Case 5. Anything else where the clock is progressing correctly
    else{
      last_time = timestamp;
      return clock_counter;
    }
  }

  // Second, is this number less than the previous?
  else if(timestamp < last_time){

    // Case 1. Genuine clock reset. under 5s is false and over 15s is true
    if(timestamp < 5e8 && !seen_under_5 && seen_over_15){
      seen_under_5 = true;
      seen_over_15 = false;
      last_time = timestamp;
      clock_counter++;
      return clock_counter;
    }

    // Case 2: Any other jitter within the 21 seconds, just return
    else{
      return clock_counter;
    }
  }
  else{
    fLog->Entry(MongoLog::Warning,
      "Board %i something odd in your clock counters. t_new: %i, last_time: %i, over_15: %i, under_5: %i",
		fBID, timestamp, last_time, seen_over_15, seen_under_5);
    // Counter equal to last time, so we're happy and keep the same counter
    return clock_counter;
  }  
}

int V1724::WriteRegister(unsigned int reg, unsigned int value){
  u_int32_t write=0;
  write+=value;
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

int64_t V1724::ReadMBLT(unsigned int *&buffer){
  // Initialize
  int64_t blt_bytes=0;
  int nb=0,ret=-5;
  // The best-equipped V1724E has 4MS/channel memory = 8 MB/channel
  // the other, V1724G, has 512 MS/channel = 1MB/channel
  //unsigned int BLT_SIZE=8388608; //8*8388608; // 8MB buffer size
  unsigned int BLT_SIZE=524288;
  std::vector<u_int32_t*> transferred_buffers;
  std::vector<u_int32_t> transferred_bytes;

  int count = 0;
  do{

    // Reserve space for this block transfer
    u_int32_t* thisBLT = new u_int32_t[BLT_SIZE/sizeof(u_int32_t)];
    
    try{
      ret = CAENVME_FIFOBLTReadCycle(fBoardHandle, fBaseAddress,
				     ((unsigned char*)thisBLT),
				     BLT_SIZE, cvA32_U_MBLT, cvD64, &nb);
    }catch(std::exception E){
      std::cout<<fBoardHandle<<" sucks"<<std::endl;
      std::cout<<"BLT_BYTES: "<<blt_bytes<<std::endl;
      std::cout<<"nb: "<<nb<<std::endl;
      std::cout<<E.what()<<std::endl;
      throw;
    };
    if( (ret != cvSuccess) && (ret != cvBusError) ){
      fLog->Entry(MongoLog::Error,
		  "Board %i read error after %i reads: (%i) and transferred %i bytes this read",
		  fBID, count, ret, nb);

      // Delete all reserved data and fail
      delete[] thisBLT;
      for(unsigned int x=0;x<transferred_buffers.size(); x++)
	delete[] transferred_buffers[x];
      return -1;
    }

    count++;
    blt_bytes+=nb;
    transferred_buffers.push_back(thisBLT);
    transferred_bytes.push_back(nb);

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
  if(blt_bytes>0){
    buffer = new u_int32_t[blt_bytes/sizeof(u_int32_t)];
    u_int32_t bytes_copied = 0;
    for(unsigned int x=0; x<transferred_buffers.size(); x++){
      std::memcpy(((unsigned char*)buffer)+bytes_copied,
		  transferred_buffers[x], transferred_bytes[x]);
      bytes_copied += transferred_bytes[x];
    }
  }
  for(unsigned int x=0;x<transferred_buffers.size(); x++)
    delete[] transferred_buffers[x];
  return blt_bytes;
  
}

int V1724::LoadDAC(std::vector<u_int16_t> &dac_values){
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

int V1724::End(){
  if(fBoardHandle>=0)
    CAENVME_End(fBoardHandle);
  fBoardHandle=fLink=fCrate=fBID=-1;
  fBaseAddress=0;
  return 0;
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
