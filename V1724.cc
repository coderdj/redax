#include "V1724.hh"


V1724::V1724(MongoLog  *log, Options *options){
  fOptions = options;
  fBoardHandle=fLink=fCrate=fBID=-1;
  fBaseAddress=0;
  fLog = log;

  fNChannels = 8;
  fAqCtrlRegister = 0x8100;
  fAqStatusRegister = 0x8104;
  fSwTrigRegister = 0x8108;
  fResetRegister = 0xEF24;
  fChStatusRegister = 0x1088;
  fChDACRegister = 0x1098;

  DataFormatDefinition = {
    {"channel_mask_msb_idx", NULL},
    {"channel_mask_msb_mask", NULL},
    {"channel_header_words", 2},
    {"ns_per_sample", 10},
    {"ns_per_clk", 10},
    // Channel indices are given relative to start of channel
    // i.e. the channel size is at index '0'
    {"channel_time_msb_idx", NULL},
    {"channel_time_msb_mask", NULL},
    
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
  return MonitorRegister(fAqStatusRegister, 0x4, 1000, 1000, 0x0);
}
u_int32_t V1724::GetAcquisitionStatus(){
  return ReadRegister(fAqStatusRegister);
}


int V1724::Init(int link, int crate, int bid, unsigned int address){
	  
  int a = CAENVME_Init(cvV2718, link, crate, &fBoardHandle);
  if(a != cvSuccess){
    cout<<"Failed to init board, error code: "<<a<<", handle: "<<fBoardHandle<<
      " at link "<<link<<" and bdnum "<<crate<<endl;
    fBoardHandle = -1;
    return -1;
  }
  fLog->Entry(MongoLog::Debug, "Initialized board %i with handle %i (link/crate)(%i/%i)",
	      bid, fBoardHandle, link, crate);  

  fLink = link;
  fCrate = crate;
  fBID = bid;
  fBaseAddress=address;
  cout<<"Successfully initialized board at "<<fBoardHandle<<endl;
  clock_counter = 0;
  last_time = 0;
  seen_over_15 = false;
  seen_under_5 = true; // starts run as true
  return 0;
}

u_int32_t V1724::GetHeaderTime(u_int32_t *buff, u_int32_t size){
  u_int32_t idx = 0;
  //std::cout<<"Size is: "<<size<<std::endl;
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
      "Something odd in your clock counters. t_new: %i, last_time: %i, over_15: %i, under_5: %i",
		timestamp, last_time, seen_over_15, seen_under_5);
    // Counter equal to last time, so we're happy and keep the same counter
    return clock_counter;
  }  
}

int V1724::WriteRegister(unsigned int reg, unsigned int value){
  //std::cout<<"Writing reg:val: "<<hex<<reg<<":"<<value<<dec<<std::endl;
  u_int32_t write=0;
  write+=value;
  if(CAENVME_WriteCycle(fBoardHandle, fBaseAddress+reg,
			&write,cvA32_U_DATA,cvD32) != cvSuccess){
    fLog->Entry(MongoLog::Warning,
		"Failed to write register 0x%04x to board %i with value %08x (handle %i)",
		reg, fBID, value, fBoardHandle);
    return -1;
  }
  fLog->Entry(MongoLog::Local, "Board %i wrote register 0x%04x with value 0x%04x",
	      fBID, reg, value);
  
  return 0;
}

unsigned int V1724::ReadRegister(unsigned int reg){
  unsigned int temp;
  int ret = -100;
  if((ret = CAENVME_ReadCycle(fBoardHandle, fBaseAddress+reg, &temp,
			      cvA32_U_DATA, cvD32)) != cvSuccess){
    fLog->Entry(MongoLog::Warning,
		"Read returned: %i (ret) 0x%08x (val) for reg 0x%04x on board %i",
		ret, temp, reg, fBID);
    return 0xFFFFFFFF;
  }
  fLog->Entry(MongoLog::Local, "Board %i read register 0x%04x as value 0x%04x",
              fBID, reg, temp);
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
  vector<u_int32_t*> transferred_buffers;
  vector<u_int32_t> transferred_bytes;

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
		  "Read error in board %i after %i reads: (%i) and transferred %i bytes this read",
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

int V1724::ConfigureBaselines(vector <u_int16_t> &end_values,
			      int nominal_value, int ntries){
  // The point of this function is to set the voltage offset per channel such
  // that the baseline is at exactly 16000 (or whatever value is set in the
  // config file). The DAC seems to be a very sensitive thing and there
  // are some strategically placed sleep statements (placed via trial, error,
  // and tears) throughout the code. Take care if changing things here.


  // Initial parameters:
  int adjustment_threshold = 5;
  int current_iteration=0;
  int repeat_this_many=5;
  int triggers_per_iteration = 1;
  int rebin_factor_log = 2;  //log base 2.
  int nbins = 1 << (14 - rebin_factor_log);
  // take all counts within this many bins of the max
  int bins_around_max = 3;
  // the counts in these bins must be this fraction of total counts
  double fraction_around_max = 0.8;

  std::vector<u_int32_t> hist(nbins);
  // some iterators to handle looping through the histogram
  auto beg_it = hist.begin();
  auto max_it = beg_it;
  auto end_it = hist.end();
  // iterators for the region around the max
  auto max_start = max_it;
  // +1 for exclusive endpoint
  auto max_end = max_it;

  // Determine starting values. If the flag 'start_provided' is set then the
  // initial argument vector already has our start values. If not then we can
  // make a decent guess here.
  u_int32_t starting_value = (0x43f8 - nominal_value)*(0xffff)/(0x3fff)*0.95;

  vector<u_int16_t> dac_values(fNChannels, starting_value);
  if(end_values[0]!=0){ // use start values if sent
    dac_values = end_values;
  }
  std::stringstream msg;
  msg << "Found starting values for digi " << fBID << " BLs:" << std::hex;
  for (auto& v : dac_values) msg << " 0x" << v << ",";
  fLog->Entry(MongoLog::Local, msg.str());

  vector<int> channel_finished(fNChannels, 0);
  vector<bool> update_dac(fNChannels, true);

  // Load up the DAC values
  if(LoadDAC(dac_values, update_dac)!=0){
    fLog->Entry(MongoLog::Error, "Digitizer %i failed to load DAC in baseline routine", fBID);
    return -2;
  }

  // ****************************
  // Main loop
  // ****************************
  while(current_iteration < ntries){

    fLog->Entry(MongoLog::Local, "Baseline iteration %i of %i possible for board %i",
		current_iteration, ntries, fBID);

    bool breakout=true;
    for(unsigned int x=0; x<channel_finished.size(); x++){
      if(channel_finished[x]<repeat_this_many)
	breakout=false;
    }
    if(breakout){
      fLog->Entry(MongoLog::Local,
		  "Baselines report all channels finished for board %i", fBID);
      break;
    }
    // enable adc
    WriteRegister(fAqCtrlRegister,0x4);//x24?
    if(MonitorRegister(fAqStatusRegister, 0x4, 1000, 1000) != true){
      fLog->Entry(MongoLog::Warning, "Timed out waiting for acquisition to start in baselines");
      return -1;
    }
    usleep(1000);
    //write trigger
    for(int ntrig=0; ntrig<triggers_per_iteration; ntrig++){
      WriteRegister(fSwTrigRegister,0x1);
      usleep(1000);                 // Give time for event?
    }

    // disable adc
    WriteRegister(fAqCtrlRegister,0x0);//x24?
    usleep(1000);

    // Read data
    u_int32_t *buff = NULL;
    int size = 0;
    size = ReadMBLT(buff);
    // Check for mal formed data
    if(size>0 && size<=16){
      std::cout<<"Delete undersized buffer ("<<size<<")"<<std::endl;
      delete[] buff;
      continue;
    }
    if(size == 0){
      std::cout<<"No event though board "<<fBID<<
	" said there would be one"<<std::endl;
      if(buff != NULL) delete[] buff;
      continue;
    }
    if(size < 0) // There's a read error
      break;

    fLog->Entry(MongoLog::Local,
		"I just got %i bytes data from %i triggers in board %i. Reasonable?",
		size, triggers_per_iteration, fBID);

    // Now we're going to acquire 'n' triggers
    std::vector<double>baseline_per_channel(fNChannels, 0);
    std::vector<double>good_triggers_per_channel(fNChannels, 0);

    // Parse
    unsigned int idx = 0;
    while(idx < size/sizeof(u_int32_t)){
      if(buff[idx]>>28==0xA){ // header

	u_int32_t words_in_event = buff[idx]&0xFFFFFFF;
	u_int32_t cmask = buff[idx+1]&0xFF;
        if (DataFormatDefinition["channel_mask_msb_idx"] != NULL) {
          // fill in the v1730 stuff here
        }
	u_int32_t channel_words = 0;
	u_int32_t n_chan = __builtin_popcount(cmask);
	if(n_chan > 0)
	  channel_words = (words_in_event-4)/n_chan - DataFormatDefinition["channel_header_words"];

	// We should track and log how often this happens. Seems rare but wtf.
	if(buff[idx+1]&0x4000000){
	  fLog->Entry(MongoLog::Local,
                "Hoppla! Board FAIL bit set for digitizer %i.", fBID);
	  idx += 4;
	  continue;
	}

	idx += 4;
	// Loop through channels
	for(unsigned int channel=0; channel<fNChannels; channel++){
	  if(!((cmask>>channel)&1))
            continue;
	  float baseline = -1.;

	  // Sometimes for some reason idx is bigger than size.
	  if(idx > (u_int32_t)(size)){
	    fLog->Entry(MongoLog::Local,
		"Found bad buffer in board %i with size %i but attempted to access %i.",
		fBID, size, idx);
	    break;
	  }

	  idx += DataFormatDefinition["channel_header_words"];
	  if(channel_finished[channel]>=repeat_this_many){
	    idx+=channel_words;
	    continue;
	  }
	  if(idx + channel_words > (u_int32_t)(size)){
	    fLog->Entry(MongoLog::Local,
			"Found bad channel size %i in board %i for payload with size %i.",
			channel_words, fBID, size);
	    break;
	  }
	  hist.assign(hist.size(), 0);
	  for(unsigned int w=0; w<channel_words; w++){
            u_int16_t val0 = buff[idx+w]&0xFFFF, val1 = (buff[idx+w]>>16)&0xFFFF;
	    if((val0==0) || (val1==0)){
	      // The presence of a 0 is clearly a symptom of some odd issue. Seems
	      // to only happen in the baseline program, so we're just gonna
	      // hit the old 'abort' button and get out of here
	      continue;
	    }
            hist[val0>>rebin_factor_log]++;
            hist[val1>>rebin_factor_log]++;
	  }
	  if(idx + channel_words > size/sizeof (u_int32_t)) break; // this should skip to next header
	  idx += channel_words;
          for (auto it = beg_it; it < end_it; it++)
            if (*it > *max_it) max_it = it;

          max_start = std::max(max_it - bins_around_max, beg_it);
          // +1 for exclusive endpoint
          max_end = std::min(max_it + bins_around_max + 1, end_it);
          // use some fancy c++ algorithms because why not
          // double-cast because ratios
          double counts_total = std::reduce(beg_it, end_it);
          double counts_around_max = std::reduce(max_start, max_end);
          if (counts_around_max/counts_total < fraction_around_max) {
            std::cout << "Only " << (int)counts_around_max << " counts out of "
                << (int)counts_total << " on board " << fBID << " ch " << channel
                << " are near the max\n";
            continue;
          }
          baseline = 0;
          // calculate the weighted average for the baseline
          for (auto it = max_start; it < max_end; it++)
            baseline += ((it - beg_it)<<rebin_factor_log)*(*it);
          baseline /= counts_around_max;

	  // Add to total
	  baseline_per_channel[channel]+= baseline;
	  good_triggers_per_channel[channel]+=1.;
	} // end for loop through channels
      } // end if header
      else
	idx++;
    }// end parse data
    delete[] buff;

    // Get average from total
    for(int channel=0; channel<fNChannels; channel++)
      baseline_per_channel[channel]/=good_triggers_per_channel[channel];

    // Compute update to baseline if any
    // Time for the **magic**. We want to see how far we are off from nominal and
    // adjust up and down accordingly. We will always adjust just a tiny bit
    // less than we think we need to to avoid getting into some overshoot
    // see-saw type loop where we never hit the target.
    for(int channel=0; channel<fNChannels; channel++){
      if(channel_finished[channel]>=repeat_this_many)
	continue;
      if(good_triggers_per_channel[channel]==0)
	continue;

      //float absolute_unit = float(0xffff)/float(0x3fff);
      float fudge_factor = 0.8;
      //int adjustment = .5*int(absolute_unit*((float(baseline_per_channel[channel])-
	//				      float(nominal_value))));
      float off_by = nominal_value - baseline_per_channel[channel];
      if(abs(off_by) < adjustment_threshold){
	channel_finished[channel]++;
	std::cout<<"Channel "<<channel<<" converging at step "<<
	  channel_finished[channel]<<"/5"<<std::endl;
      }
      else{
        int adjustment = (0xffff)/(0x3fff)*off_by*fudge_factor;
	channel_finished[channel]=0;
	update_dac[channel] = true;
        // 0xf3c is about the linear limit, let's stay away from it
        dac_values[channel] = std::clamp(dac_values[channel]+adjustment, 0x1000, 0xffff);
        if ((dac_values[channel] == 0x1000) || (dac_values[channel] == 0xffff)) {
          std::cout<<"Channel "<<channel<<" DAC clamped to "<<dac_values[channel]<< '\n';
        }
        else {
	  std::cout<<"Had channel "<<channel<<" at "<<dac_values[channel]-adjustment
	    <<" but now it's at "<<dac_values[channel]<<" (adjustment) BL: "<<
	    baseline_per_channel[channel]<<
	    " ("<<good_triggers_per_channel[channel]<<" iterations)"<<std::endl;
	}
      }
    } // End final channel adjustment
    current_iteration++;
    usleep(5000);
    // Load DAC
    if(LoadDAC(dac_values, update_dac)!=0){
      fLog->Entry(MongoLog::Error, "Digitizer %i failed to load DAC in baseline routine",
		  fBID);
      return -2;
    }


  } // end while(current_iteration < ntries)

  for(unsigned int x=0; x<channel_finished.size(); x++){
    if(channel_finished[x]<2){ // Be a little more lenient in case it's just starting to converge
      fLog->Entry(MongoLog::Message,
		  "Baseline routine did not finish for channel %i (and maybe others)", x);
      return -1;
    }
  }

  end_values = dac_values;
  return 0;
}


int V1724::LoadDAC(vector<u_int16_t>dac_values, vector<bool> &update_dac){
  // Loads DAC values into registers

  for(unsigned int x=0; x<dac_values.size(); x++){
    if(x>fNChannels || update_dac[x]==false) // oops
      continue;

    // Give the DAC time to be set if needed
    usleep(1000);

    if(MonitorRegister((fChStatusRegister)+(0x100*x), 0x4, 100, 1000, 0) != true){
      fLog->Entry(MongoLog::Error, "Timed out waiting for channel %i in DAC setting", x);
      return -1;
    }

    // Now write channel DAC values
    if(WriteRegister((fChDACRegister)+(0x100*x), dac_values[x])!=0){
      fLog->Entry(MongoLog::Error, "Failed writing DAC 0x%04x in channel %i",
		  dac_values[x], x);
      return -1;
    }

    // Give the DAC time to be set if needed
    usleep(1000);

    if(MonitorRegister((fChStatusRegister)+(0x100*x), 0x4, 100, 1000, 0) != true){
      fLog->Entry(MongoLog::Error, "Timed out waiting for channel %i after DAC setting", x);
      return -1;
    }
  }
  // Sleep a bit because apparently checking the register means nothing and you
  // gotta wait a little for the actual voltage to be updated
  usleep(5000);
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
  std::cout<<"MonitorRegister failed for "<<hex<<reg<<" with mask "<<
    mask<<" and register value "<<rval<<"... couldn't get "<<val<<dec<<
    std::endl;
  return false;
}
