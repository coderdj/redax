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
  if(CAENVME_WriteCycle(fBoardHandle, fBaseAddress+reg,
			&write,cvA32_U_DATA,cvD32) != cvSuccess){
    fLog->Entry(MongoLog::Warning,
		"Board %i failed to write register 0x%04x with value %08x (handle %i)",
		fBID, reg, value, fBoardHandle);
    return -1;
  }
  //fLog->Entry(MongoLog::Local, "Board %i wrote register 0x%04x with value 0x%04x",
	//      fBID, reg, value);
  
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
  //fLog->Entry(MongoLog::Local, "Board %i read register 0x%04x as value 0x%04x",
  //            fBID, reg, temp);
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

int V1724::ConfigureBaselines(std::vector<u_int16_t> &dac_values,
        std::map<std::string, std::vector<double>> &cal_values,
	int nominal_value, int ntries, bool &calibrate){
  // The point of this function is to set the voltage offset per channel such
  // that the baseline is at exactly 16000 (or whatever value is set in the
  // config file). The DAC seems to be a very sensitive thing and there
  // are some strategically placed sleep statements (placed via trial, error,
  // and tears) throughout the code. Take care if changing things here.

  // Initial parameters:
  int adjustment_threshold = 10; // baseline units
  int repeat_this_many=3;
  int rebin_factor_log = 1;  //log base 2.
  int nbins = 1 << (14 - rebin_factor_log);
  int min_adjustment = 8; // dac units
  // take all counts within this many bins of the max
  int bins_around_max = 3;
  // the counts in these bins must be this fraction of total counts
  double fraction_around_max = 0.8, counts_around_max(0), counts_total(0), baseline(0);
  double slope(0), yint(0);
  u_int32_t words_in_event(0), channel_mask(0), words_per_channel(0);
  int channels_in_event(0), idx(0);
  bool redo_iter(false);

  std::vector<int> hist(nbins);
  // some iterators to handle looping through the histogram
  auto beg_it = hist.begin();
  auto max_it = beg_it;
  auto end_it = hist.end();
  // iterators for the region around the max
  auto max_start = max_it;
  // +1 for exclusive endpoint
  auto max_end = max_it;

  std::array<int, 3> DAC_calibration = {60000, 30000, 6000};
  std::vector<u_int16_t> min_dac(fNChannels);
  u_int16_t max_dac(0xffff), val0(0), val1(0);
  
  // B = sum(x^2), C = sum(1), F = sum(x)
  double B(4536000000), C(DAC_calibration.size()), D(0), E(0), F(96000);

  std::vector<std::vector<double>> bl_per_channel(fNChannels, std::vector<double>(3+ntries));

  dac_values = std::vector<u_int16_t>(fNChannels);
  if (!calibrate) { // calibration already done, values are usable
    if (cal_values["yint"].size() < 1) { // something wonky happened
        calibrate = false;
    } else {
      for (unsigned ch = 0; ch < fNChannels; ch++) {
        if (cal_values["yint"][ch] > 0x3fff) {
          min_dac[ch] = (0x3fff - cal_values["yint"][ch])/cal_values["slope"][ch];
        } else {
          min_dac[ch] = 0;
        }
        dac_values[ch] = std::clamp(dac_values[ch], min_dac[ch], max_dac);
        if ((dac_values[ch] == min_dac[ch]) || (dac_values[ch] == max_dac)) {
	  fLog->Entry(MongoLog::Local, "Board %i channel %i clamped dac to 0x%04x",
	      fBID, ch, dac_values[ch]);
        }
      }
    }
  }

  std::vector<int> channel_finished(fNChannels, 0);
  std::vector<bool> update_dac(fNChannels, true);

  u_int32_t* buffer;
  int bytes_read;

  int steps_repeated = 0;
  int max_repeats = 10;

  for (int step = 0; step < 3 + ntries; step++) {
    if (std::all_of(channel_finished.begin(), channel_finished.end(),
	  [=](int i) {return i >= repeat_this_many;})) {
      fLog->Entry(MongoLog::Local,
		  "Board %i baselines report all channels finished", fBID);
      break;
    }
    if ((step < 3) && (!calibrate)) continue;
    fLog->Entry(MongoLog::Local, "Board %i baseline iteration %i/%i",
		fBID, step, ntries);
    if ((step < 3))
      dac_values.assign(dac_values.size(), DAC_calibration[step]);
    if(LoadDAC(dac_values, update_dac)){
      fLog->Entry(MongoLog::Warning, "Board %i failed to load DAC in baseline calibration", fBID);
      return -2;
    }
    WriteRegister(fAqCtrlRegister,0x4);//x24?
    if(MonitorRegister(fAqStatusRegister, 0x4, 1000, 1000) != true){
      fLog->Entry(MongoLog::Warning, "Board %i timed out waiting for acquisition to start in baselines", fBID);
      return -1;
    }
    usleep(5000);
    //write trigger
    WriteRegister(fSwTrigRegister,0x1);
    usleep(5000);                 // Give time for event?

    // disable adc
    WriteRegister(fAqCtrlRegister,0x0);//x24?
    usleep(5000);

    bytes_read = ReadMBLT(buffer);
    if (bytes_read < 0) {
        fLog->Entry(MongoLog::Warning, "Board %i baselines read %i bytes",
                fBID, bytes_read);
        return -2;
    }
    else if ((0<=bytes_read) && (bytes_read<=16)) {
      std::cout << "Buffer undersized ("<<bytes_read<<")\n";
      delete[] buffer;
      step--;
      steps_repeated++;
      if (steps_repeated > max_repeats) {
	fLog->Entry(MongoLog::Error, "Board %i baselines keeps failing readouts", fBID);
	return -1;
      }
      continue;
    }
    
    idx = 0;
    while ((idx * sizeof(u_int32_t) < bytes_read) && (idx >= 0)) {
      if ((buffer[idx]>>28) == 0xA) { // start of header
	words_in_event = buffer[idx]&0xFFFFFFF;
        if (words_in_event == 4) {
          idx += 4;
          continue;
        }
	channel_mask = buffer[idx+1]&0xFF;
	if (DataFormatDefinition["channel_mask_msb_idx"] != -1) {
	  // fill in V1730 stuff here
	}
	channels_in_event = std::bitset<16>(channel_mask).count();
	words_per_channel = (words_in_event - 4)/channels_in_event - DataFormatDefinition["channel_header_words"];

	idx += 4;
	for (unsigned ch = 0; ch < fNChannels; ch++) {
	  if (!(channel_mask & (1 << ch))) continue;
	  idx += DataFormatDefinition["channel_header_words"];
	  hist.assign(hist.size(), 0);
	  for (unsigned w = 0; w < words_per_channel; w++) {
            val0 = buffer[idx+w]&0xFFFF;
            val1 = (buffer[idx+w]>>16)&0xFFFF;
            if (val0 * val1 == 0) continue;
	    hist[val0>>rebin_factor_log]++;
	    hist[val1>>rebin_factor_log]++;
	  }
	  idx += words_per_channel;
          for (auto it = beg_it; it < end_it; it++)
            if (*it > *max_it) max_it = it;

          max_start = std::max(max_it - bins_around_max, beg_it);
          // +1 for exclusive endpoint
          max_end = std::min(max_it + bins_around_max + 1, end_it);
          // use some fancy c++ algorithms because why not
          counts_total = std::accumulate(beg_it, end_it, 0.);
          counts_around_max = std::accumulate(max_start, max_end, 0.);
	  if (counts_around_max/counts_total < fraction_around_max) {
	    fLog->Entry(MongoLog::Local, "Bd %i: %d out of %d are around max, ch %i max_i %i",
                fBID,counts_around_max,counts_total,ch,(max_it - beg_it)<<rebin_factor_log);
	    redo_iter=true;
	  }
          if (counts_total/words_per_channel < 1.5) // too many zeros
              redo_iter = true;
          baseline = 0;
          // calculate the weighted average for the baseline
          for (auto it = max_start; it < max_end; it++)
            baseline += ((it - beg_it)<<rebin_factor_log)*(*it);
          baseline /= counts_around_max;
	  bl_per_channel[ch][step] = baseline;
	} // end of for channels
      } // end of header
      else
	idx++;
    } // end of while
    delete[] buffer;
    if (redo_iter) {
      redo_iter=false;
      step--;
      steps_repeated++;
      if (steps_repeated > max_repeats) {
	fLog->Entry(MongoLog::Error, "Board %i baselines keeps failing readouts", fBID);
	return -1;
      }
      continue;
    }

    if (step < 2) continue;
    if (step == 2) {
      cal_values["slope"] = std::vector<double>(fNChannels);
      cal_values["yint"] = std::vector<double>(fNChannels);
      // ****************
      // First: calibrate
      // ****************
      for (unsigned ch = 0; ch < fNChannels; ch++) {
        // basic chi-squared minimization
	D = E = 0;
        for (int i = 0; i < 3; i++) {
	  D += DAC_calibration[i]*bl_per_channel[ch][i];
          E += bl_per_channel[ch][i];
	}
        cal_values["slope"][ch] = slope = (C*D-E*F)/(B*C-F*F);
	cal_values["yint"][ch] = yint = (B*E-D*F)/(B*C-F*F);
        fLog->Entry(MongoLog::Debug, "Board %i ch %i baseline calibration: %.3f/%.1f",
	  fBID, ch, slope, yint);

        dac_values[ch] = (nominal_value - yint)/slope;
	if (cal_values["yint"][ch] > 0x3fff) {
          min_dac[ch] = (0x3fff - yint)/slope;
	} else {
          min_dac[ch] = 0;
	}
        dac_values[ch] = std::clamp(dac_values[ch], min_dac[ch], max_dac);
	if ((dac_values[ch] == min_dac[ch]) || (dac_values[ch] == max_dac)) {
          fLog->Entry(MongoLog::Local, "Board %i calibration for channel %i clamped to %04x",
	      fBID, ch, dac_values[ch]);
	}
      }
      calibrate=false;
    } else {
      // *********
      // Next: fit
      // *********
      for (unsigned ch = 0; ch < fNChannels; ch++) {
	if (channel_finished[ch]>=repeat_this_many) continue;

	float off_by = nominal_value - bl_per_channel[ch][step];
	if (abs(off_by) < adjustment_threshold) {
	  channel_finished[ch]++;
	  update_dac[ch] = false;
	  continue;
	} else {
	  update_dac[ch] = true;
	}

	int adjustment = off_by * cal_values["slope"][ch];
	if (abs(adjustment) < min_adjustment)
	  adjustment = std::copysign(min_adjustment, adjustment);
	fLog->Entry(MongoLog::Local, "Board %i channel %i dac %04x bl %.1f adjust %i iter %i",
	  fBID, ch, dac_values[ch], bl_per_channel[ch].back(), adjustment, step);
	dac_values[ch] += adjustment;
	dac_values[ch] = std::clamp(dac_values[ch], min_dac[ch], max_dac);
	if ((dac_values[ch] == min_dac[ch]) || (dac_values[ch] == max_dac)) {
	  fLog->Entry(MongoLog::Local, "Board %i channel %i clamped dac to %04x",
	    fBID, ch, dac_values[ch]);
	}

      } // end for channels
    } // end of if calibrate/fit
  } // end of iteration

  if (std::any_of(channel_finished.begin(), channel_finished.end(),
	  [=](int i) {return i < repeat_this_many;})) {
    fLog->Entry(MongoLog::Message,
            "Board %i baselines didn't finish for at least one channel", fBID);
    return -1; // something didn't finish
  }

  return 0;
}


int V1724::LoadDAC(std::vector<u_int16_t> &dac_values, std::vector<bool> &update_dac){
  // Loads DAC values into registers
  for(unsigned int x=0; x<dac_values.size(); x++){
    if(x>=fNChannels || update_dac[x]==false) // oops
      continue;

    // Now write channel DAC values
    if(WriteRegister((fChDACRegister)+(0x100*x), dac_values[x])!=0){
      fLog->Entry(MongoLog::Error, "Board %i failed writing DAC 0x%04x in channel %i",
		  fBID, dac_values[x], x);
      return -1;
    }

    // Give the DAC time to be set if needed
    usleep(5000);

  }
  // Sleep a bit because the DAC responds kinda slow
  usleep(200000);
  return 0;
}

int V1724::End(){
  if(fBoardHandle>=0)
    CAENVME_End(fBoardHandle);
  fBoardHandle=fLink=fCrate=fBID=-1;
  fBaseAddress=0;
  return 0;
}

bool V1724::MonitorRegister(u_int32_t reg, u_int32_t mask, int ntries, int sleep, u_int32_t val){
  int counter = 0;
  u_int32_t rval = 0;
  if(val == 0) rval = 0xffffffff;
  while(counter < ntries){
    rval = ReadRegister(reg);
    if(rval == 0xffffffff)
      return false;
    if((val == 1 && (rval&mask)) || (val == 0 && !(rval&mask)))
      return true;
    counter++;
    usleep(sleep);
  }
  fLog->Entry(MongoLog::Warning,"Board %i MonitorRegister failed for 0x%04x with mask 0x%04x and register value 0x%04x, wanted 0x%04x",
          fBID, reg, mask, rval,val);
  return false;
}
