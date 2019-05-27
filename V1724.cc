#include "V1724.hh"

V1724::V1724(MongoLog  *log){
  fBoardHandle=fLink=fCrate=fBID=-1;
  fBaseAddress=0;
  fLog = log;
}
V1724::~V1724(){
  End();
}

int V1724::Init(int link, int crate, int bid, unsigned int address=0){

  /*
  // I am hoping that minesweeper will no longer be needed
  // MINESWEEPER
  stringstream command;
  command<<"(cd /home/xedaq/minesweeper && echo `./minesweeper -l "<<
    link<<" -c "<<crate<<"`)";
  cout<<"Sending command: "<<command.str()<<endl;
  int retsys = system(command.str().c_str());
  cout<<"Returned: "<<retsys<<endl;
  usleep(1000);
  */
	  
  int a = CAENVME_Init(cvV2718, link, crate, &fBoardHandle);
  if(a != cvSuccess){
    cout<<"Failed to init board, error code: "<<a<<", handle: "<<fBoardHandle<<
      " at link "<<link<<" and bdnum "<<crate<<endl;
    fBoardHandle = -1;
    return -1;
  }
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
    std::stringstream err;
    err<<"Something odd in your clock counters. t_new: "<<timestamp<<
    " last time: "<<last_time<<" over 15: "<<seen_over_15<<
    " under 5: "<<seen_under_5;
    fLog->Entry(err.str(), MongoLog::Warning);
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
    std::stringstream err;
    err<<"Failed to write register 0x"<<hex<<reg<<dec<<" to board "<<fBID<<
      " with value "<<hex<<value<<dec<<" board handle "<<fBoardHandle<<endl;
    fLog->Entry(err.str(), MongoLog::Warning);
    return -1;
  }
  // std::cout<<hex<<"Wrote register "<<reg<<" with value "<<value<<" for board "<<dec<<fBID<<std::endl;  
  usleep(5000); // don't ask
  return 0;
}

unsigned int V1724::ReadRegister(unsigned int reg){
  unsigned int temp;
  int ret = -100;
  if((ret = CAENVME_ReadCycle(fBoardHandle, fBaseAddress+reg, &temp,
			      cvA32_U_DATA, cvD32)) != cvSuccess){
    std::stringstream err;
    std::cout<<"Read returned: "<<ret<<" "<<hex<<temp<<std::endl;
    err<<"Failed to read register 0x"<<hex<<reg<<dec<<" on board "<<fBID<<
      ": "<<ret<<endl;
    fLog->Entry(err.str(), MongoLog::Warning);
    return 0xFFFFFFFF;
  }
  usleep(5000);
  return temp;
}

u_int32_t V1724::ReadMBLT(unsigned int *&buffer){
  // Initialize
  unsigned int blt_bytes=0;
  int nb=0,ret=-5;
  // The best-equipped V1724E has 4MS/channel memory = 8 MB/channel
  // the other, V1724G, has 512 MS/channel = 1MB/channel
  //unsigned int BLT_SIZE=8388608; //8*8388608; // 8MB buffer size
  unsigned int BLT_SIZE=16384; //524288;
  unsigned int BUFFER_SIZE = 8388608; // 8 MB memory of digi (even allocating more than needed)
  u_int32_t *tempBuffer = new u_int32_t[BUFFER_SIZE];

  int count = 0;
  do{
    try{
      ret = CAENVME_BLTReadCycle(fBoardHandle, fBaseAddress,
				     ((unsigned char*)tempBuffer)+blt_bytes,
				     BLT_SIZE, cvA32_U_BLT, cvD32, &nb);
    }catch(std::exception E){
      std::cout<<fBoardHandle<<" sucks"<<std::endl;
      std::cout<<"BLT_BYTES: "<<blt_bytes<<std::endl;
      std::cout<<"nb: "<<nb<<std::endl;
      std::cout<<E.what()<<std::endl;
      throw;
    };
    if( (ret != cvSuccess) && (ret != cvBusError) ){
      stringstream err;
      err<<"Read error in board "<<fBID<<" after "<<count<<" reads: "<<dec<<ret;
      fLog->Entry(err.str(), MongoLog::Error);
      u_int32_t data=0;
      data = ReadRegister(0x8104);
      std::cout<<"Board status: "<<hex<<data<<dec<<std::endl;
      delete[] tempBuffer;
      return 0;
    }

    count++;
    blt_bytes+=nb;

    if(blt_bytes>BUFFER_SIZE){
      stringstream err;
      err<<"You managed to transfer more data than fits on board."<<
	"Transferred: "<<blt_bytes<<" bytes, Buffer: "<<BUFFER_SIZE<<" bytes.";
      fLog->Entry(err.str(), MongoLog::Error);
      
      delete[] tempBuffer;
      return 0;
    }
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
    buffer = new u_int32_t[blt_bytes/(sizeof(u_int32_t))];
    std::memcpy(buffer, tempBuffer, blt_bytes);
  }
  delete[] tempBuffer;
  return blt_bytes;
  
}

int V1724::ConfigureBaselines(vector <u_int16_t> &end_values,
			     int nominal_value, int ntries){
  // Adjust DAC offset voltages so that channels are at 'nominal_value' baseline
  // Contains lots of voodoo magic and snake-oil methods meant to avoid
  // edge case crashes, which are not understood and seem to come from nowhere

  // How close do we need to be to call it "OK". If you're using a self-trigger
  // with dynamic baseline fitting maybe just 5-10 is close enough. If you're using
  // and absolute threshold you probably want more like 1-2... but then you need
  // very clean conditions!
  int adjustment_threshold = 5;
  int current_iteration=0;
  int nChannels = 8;
  int repeat_this_many=5; // how many times it has to hit it before we stop
  int triggers_per_iteration = 10;
  
  // Determine starting values. If the flag 'start_provided' is set then the
  // initial argument vector already has our start values. If not then we can
  // make a decent guess here.
  u_int32_t starting_value = u_int32_t( (0x3fff-nominal_value)*((0.9*0xffff)/0x3fff) + 3277);
  vector<u_int16_t> dac_values(nChannels, starting_value);
  if(end_values[0]!=0 && end_values.size() == nChannels){ // use start values if sent
    std::cout<<"Found good start values for digi "<<fBID<<": ";
    for(unsigned int x=0; x<end_values.size(); x++){
      dac_values[x] = end_values[x];
      std::cout<<dac_values[x]<<" ";
    }
    std::cout<<std::endl;
  }
  vector<int> channel_finished(nChannels, 0);
  vector<bool> update_dac(nChannels, true);

  while(current_iteration < ntries){

    current_iteration++;

    // First check if maybe we're done already
    bool breakout = true;
    for(unsigned int channel=0; channel<nChannels; channel++){
      if(channel_finished[channel] >= repeat_this_many)
	continue;
      breakout=false;
    }
    if(breakout)
      break;
    
    // Reset the whole thing because we are super paranoid
    CAENVME_End(fBoardHandle);
    int a = CAENVME_Init(cvV2718, fLink, fCrate, &fBoardHandle);
    if(a != cvSuccess){
      fLog->Entry("Failed to CAEN init in baseline routine", MongoLog::Warning);
      return -1;
    }

    // Now reload all the registers we need for taking a bit of data
    int write_success = 0;
    try{
      write_success += WriteRegister(0xEF24, 0x1);       // Global reset
      write_success += WriteRegister(0xEF1C, 0x1);       // BERR
      write_success += WriteRegister(0xEF00, 0x130);      // Channel memory
      write_success += WriteRegister(0x811C, 0x110);
      write_success += WriteRegister(0x81A0, 0x200);
      write_success += WriteRegister(0x8100, 0x0);
      write_success += WriteRegister(0x800C, 0xA);
      write_success += WriteRegister(0x8098, 0x1000);
      write_success += WriteRegister(0x8000, 0x310);
      write_success += WriteRegister(0x8080, 0x1310000);
      write_success += WriteRegister(0x8034, 0x0);
      write_success += WriteRegister(0x8038, 0x1);

    }
    catch(const std::exception &e){
      std::stringstream error;
      error<<"Digitizer "<<fBID<<" CAEN fault during initial register adjustment in baseline routine";
      fLog->Entry(error.str(), MongoLog::Error);
      return -2;
    }
    // Slightly more palatable error, at least CAENVMElib is recognizing a failure
    // and not just seg faulting
    if(write_success!=0){
      std::stringstream error;
      error<<"Digitizer "<<fBID<<" unable to load registers for baselines.";
      fLog->Entry(error.str(), MongoLog::Error);
      return -2;
    }

    // Load up the DAC values
    if(LoadDAC(dac_values, update_dac)!=0){
      std::stringstream error;
      error<<"Digitizer "<<fBID<<" failed to load DAC in baseline routine.";
      fLog->Entry(error.str(), MongoLog::Error);
      return -2;
    }

    // Now we're going to acquire 'n' triggers
    std::vector<double>baseline_per_channel(nChannels, 0);
    std::vector<double>good_triggers_per_channel(nChannels, 0);
    
    // Make sure we can acquire
    if(MonitorRegister(0x8104, 0x100, 1000, 1000) != true){
      u_int32_t dat = ReadRegister(0x8178);
      fLog->Entry("Timed out waiting for board to be ready in baseline", MongoLog::Warning);
      std::cout<<"Board ready timeout: register 8174 value is "<<hex<<dat<<dec<<std::endl;
      return -1;
    }

    // Start acquisition
    WriteRegister(0x8100,0x4);//x24?   // Acq control reg
    if(MonitorRegister(0x8104, 0x4, 1000, 1000) != true){
      fLog->Entry("Timed out waiting for acquisition to start in baselines", MongoLog::Warning);
      return -1;
    }
    for(unsigned int trig=0; trig<triggers_per_iteration; trig++){

      // Send SW trigger
      WriteRegister(0x8108,0x1);    // Software trig reg
      if(MonitorRegister(0x8104, 0x8, 1000, 1000) != true){
	fLog->Entry("Timed out waiting for event ready in baselines", MongoLog::Warning);
	return -1;
      }
      // Read data
      u_int32_t *buff = NULL;
      u_int32_t size = 0;
      size = ReadMBLT(buff);
      // Check for mal formed data
      if(size>0 && size<=16){
	std::cout<<"Delete undersized buffer ("<<size<<")"<<std::endl;
	delete[] buff;
	continue;
      }
      if(size == 0){
	std::cout<<"No event though board said there would be one"<<std::endl;
	if(buff != NULL) delete[] buff;
	continue;
      }
      
      // Parse
      unsigned int idx = 0;
      while(idx < size/sizeof(u_int32_t)){
	if(buff[idx]>>20==0xA00){ // header
	  u_int32_t cmask = buff[idx+1]&0xFF;
	  idx += 4;
	  
	  // Loop through channels
	  for(unsigned int channel=0; channel<8; channel++){
	    
	    float baseline = -1.;
	    long int tbase = 0;
	    int bcount = 0;
	    unsigned int minval = 0x3fff, maxval=0;
	    
	    if(!((cmask>>channel)&1))
	      continue;
	    u_int32_t csize = buff[idx]&0x7FFFFF;
	    if(channel_finished[channel]>=5){
	      idx+=csize;
	      continue;
	    }
	    idx+=2;
	    
	    for(unsigned int i=0; i<csize-2; i++){
	      if(((buff[idx+i]&0xFFFF)==0) || (((buff[idx+i]>>16)&0xFFFF)==0))
		continue;
	      tbase += buff[idx+i]&0xFFFF;
	      tbase += (buff[idx+i]>>16)&0xFFFF;
	      bcount+=2;
	      if((buff[idx+i]&0xFFFF)<minval)
		minval = buff[idx+i]&0xFFFF;
	      if((buff[idx+i]&0xFFFF)>maxval)
		maxval = buff[idx+i]&0xFFFF;
	      if(((buff[idx+i]>>16)&0xFFFF)<minval)
		minval=(buff[idx+i]>>16)&0xFFFF;
	      if(((buff[idx+i]>>16)&0xFFFF)>maxval)
		maxval=(buff[idx+i]>>16)&0xFFFF;
	    }
	    idx += csize-2;
	    
	    // Toss if signal inside
	    if(abs(maxval-minval>50)){
	      std::cout<<"Signal in baseline, channel "<<channel
		       <<" min: "<<minval<<" max: "<<maxval<<std::endl;
	    }
	    else
	      baseline = (float(tbase) / ((float(bcount))));
	    
	    // Add to total
	    baseline_per_channel[channel]+= baseline;
	    good_triggers_per_channel[channel]+=1.;
	  } // end for loop through channels
	  delete[] buff;
	  break;
	}
	else
	  idx++;
      }// end parse data
      
    } // end for to acquire 'n' triggers
    
    // Deactivate board
    WriteRegister(0x8100, 0x0);
    
    // Get average from total
    for(unsigned int channel=0; channel<nChannels; channel++)
      baseline_per_channel[channel]/=good_triggers_per_channel[channel];

    
    // Compute update to baseline if any
    // Time for the **magic**. We want to see how far we are off from nominal and
    // adjust up and down accordingly. We will always adjust just a tiny bit
    // less than we think we need to to avoid getting into some overshoot
    // see-saw type loop where we never hit the target.
    for(unsigned int channel=0; channel<nChannels; channel++){
      float absolute_unit = float(0xffff)/float(0x3fff);      
      int adjustment = -.1*int(absolute_unit*((float(baseline_per_channel[channel])-float(nominal_value))));
      //int adjustment = int(baseline)-int(target_value);
      //std::cout<<dec<<"Adjustment: "<<adjustment<<" with threshold "<<adjustment_threshold<<std::endl;
      //std::cout<<"Baseline: "<<baseline<<" DAC tihis channel: "<<dac_values[channel]<<std::endl;
      if(abs(float(baseline_per_channel[channel])-float(nominal_value)) < adjustment_threshold){
	channel_finished[channel]++;
	std::cout<<"Channel "<<channel<<" converging at step "<<channel_finished[channel]<<"/5"<<std::endl;
      }
      else{
	channel_finished[channel]=0;
	//update_dac[channel] = true;
	if(adjustment<0 && (u_int32_t(abs(adjustment)))>dac_values[channel]){
	  dac_values[channel]=0x0;
	  std::cout<<"Channel "<<channel<<" DAC to zero"<<std::endl;
	}
	else if(adjustment>0 &&dac_values[channel]+adjustment>0xffff){
	  dac_values[channel]=0xffff;
	  std::cout<<"Channel "<<channel<<" DAC to 0xffff"<<std::endl;
	}
	else {
	  std::cout<<"Had channel "<<channel<<" at "<<dac_values[channel];
	  dac_values[channel]+=(adjustment);
	  std::cout<<" but now it's at "<<dac_values[channel]<<" (adjustment) BL: "<<baseline_per_channel[channel]<<std::endl;
	}
      }
    } // End final channel adjustment
  } // End 'iterations < max iterations'
  
  for(unsigned int x=0; x<channel_finished.size(); x++){
    if(channel_finished[x]<2){ // Be a little more lenient in case it's just starting to converge
      std::stringstream error;
      error<<"Baseline routine did not finish for channel "<<x<<" (and maybe others)."<<std::endl;
      fLog->Entry(error.str(), MongoLog::Error);
      return -1;
    }
  }
  
  end_values = dac_values;
  return 0;
    
  
}
/*
int V1724::ConfigureBaselines(vector <u_int16_t> &end_values,
			      int nominal_value, int ntries){

  // Baseline configuration routine. The V1724 has a DAC offset setting that
  // biases the ADC, allowing you to effectively move the 'zero level' of the
  // acquisition up or down. But keep in mind this is a bit limited. It's not
  // designed to correct large offsets O(~volts) (i.e. your detector ground very far
  // from ADC ground) but rather to move the baseline to the location best
  // suited to your acquisition (positive, negative, bipolar logic)

  u_int32_t target_value = nominal_value;
  int adjustment_threshold = 5;
  
  // We can adjust a DAC offset register, which is 0xffff in range, is inversely
  // proportional to the baseline position, and has ~5% overshoot on either end.
  // So we use this information to get starting values:
  u_int32_t starting_value = u_int32_t( (0x3fff-target_value)*((0.9*0xffff)/0x3fff)) + 3277;
  unsigned int nChannels = 8;
  vector<u_int16_t> dac_values(nChannels, starting_value);
  if(end_values[0]!=0 && end_values.size() == nChannels){ // use start values if sent
    std::cout<<"Found good start values for digi "<<fBID<<": ";
    for(unsigned int x=0; x<end_values.size(); x++){
      dac_values[x] = end_values[x];
      std::cout<<dac_values[x]<<" ";
    }
    std::cout<<std::endl;
  }
  vector<int> channel_finished(nChannels, 0);
  vector<bool> update_dac(nChannels, true);

  // The CAEN edge-case demons hiding somewhere between the library, PCIe card,
  // and digitizer demand a FULL RESET of the board...
  CAENVME_End(fBoardHandle);
  int a = CAENVME_Init(cvV2718, fLink, fCrate, &fBoardHandle);
  if(a != cvSuccess){
    fLog->Entry("Failed to init in the baseline routine", MongoLog::Warning);
    return -1;
  }
  
  // Now we need to load a simple configuration to the board in order to read
  // some data. try/catch cause CAENVMElib fails poorly (segfault) sometimes
  // in case the hardware has an issue.
  int write_success = 0;
  try{
    write_success += WriteRegister(0xEF24, 0x1);       // Global reset    
    write_success += WriteRegister(0xEF1C, 0x1);       // BERR 
    write_success += WriteRegister(0xEF00, 0x130);      // Channel memory
    //write_success += WriteRegister(0x8020, 0x1F4);     // Buffer size

    write_success += WriteRegister(0x811C, 0x110);
    write_success += WriteRegister(0x81A0, 0x200);
    write_success += WriteRegister(0x8100, 0x0);
    write_success += WriteRegister(0x800C, 0xA);
    write_success += WriteRegister(0x8098, 0x1000);
    write_success += WriteRegister(0x8000, 0x310);
    write_success += WriteRegister(0x8080, 0x1310000);
    write_success += WriteRegister(0x8034, 0x0);
    write_success += WriteRegister(0x8038, 0x1);

  }
  catch(const std::exception &e){
    std::stringstream error;
    error<<"Digitizer "<<fBID<<" CAEN fault during initial register adjustment in baseline routine";
    fLog->Entry(error.str(), MongoLog::Error);
    return -2;
  }

  // Slightly more palatable error, at least CAENVMElib is recognizing a failure
  // and not just seg faulting
  if(write_success!=0){
    std::stringstream error;
    error<<"Digitizer "<<fBID<<" unable to load registers for baselines.";
    fLog->Entry(error.str(), MongoLog::Error);
    return -2;
  }

  // Now we'll iterate for a while. It should be pretty quick since starting values
  // should be quite close to true.
  int currentIteration = 0;
  int maxIterations = ntries;
  while(currentIteration < maxIterations){
    currentIteration++;

    // First check if we're finished and if so get out
    bool breakout = true;
    for(unsigned int channel=0; channel<nChannels; channel++){
      if(channel_finished[channel]>=5)
        continue;
      breakout = false;
    }
    if(breakout)
      break;

    // Load DAC for this channel
    if(LoadDAC(dac_values, update_dac)!=0){
      std::stringstream error;
      error<<"Digitizer "<<fBID<<" failed to load DAC in baseline routine.";
      fLog->Entry(error.str(), MongoLog::Error);
      return -2;
    }
    
    
    //WriteRegister(0xEF28, 0x1);       // Software clear any old data
    //usleep(1000);

    // Make sure we can acquire
    if(MonitorRegister(0x8104, 0x100, 1000, 1000) != true){
      u_int32_t dat = ReadRegister(0x8178);
      fLog->Entry("Timed out waiting for board to be ready in baseline", MongoLog::Warning);
      std::cout<<"Board ready timeout: register 8174 value is "<<hex<<dat<<dec<<std::endl;
      return -1;
    }
    
    // Start acquisition
    WriteRegister(0x8100,0x4);//x24?   // Acq control reg    
    if(MonitorRegister(0x8104, 0x4, 1000, 1000) != true){
      fLog->Entry("Timed out waiting for acquisition to start in baselines", MongoLog::Warning);
      return -1;
    }
    
    // Send SW trigger
    WriteRegister(0x8108,0x1);    // Software trig reg      
    if(MonitorRegister(0x8104, 0x8, 1000, 1000) != true){
      fLog->Entry("Timed out waiting for event ready in baselines", MongoLog::Warning);
      return -1;
    }    

    // Mega paranoia. Reg 8104 has already indicated an event is ready. But let's see how
    // big this event is and print (debug step)
    u_int32_t event_size_data=0;
    event_size_data = ReadRegister(0x814C);
    std::cout<<"(BL) Next event size is gonna be: "<<dec<<event_size_data<<" 32-bit words"<<std::endl;
    
    // Read data
    u_int32_t *buff = NULL;
    u_int32_t size = 0;
    size = ReadMBLT(buff);

    // Deactivate board
    WriteRegister(0x8100, 0x0);

    // Ensure the board is no longer running before continuing
    if(MonitorRegister(0x8104, 0x4, 1000, 1000, 0x0) != true){
      fLog->Entry("Timed out waiting for acquisition to stop in baselines",
		  MongoLog::Warning);
      return -1;
    }
    
    // Check for mal formed data
    if(size>0 && size<=16){
      std::cout<<"Delete undersized buffer ("<<size<<")"<<std::endl;
      delete[] buff;      
      continue;
    }
    if(size == 0){
      std::cout<<"No event though board said there would be one"<<std::endl;
      if(buff != NULL) delete[] buff;
      continue;
    }
        
    // Parse
    unsigned int idx = 0;
    while(idx < size/sizeof(u_int32_t)){
      if(buff[idx]>>20==0xA00){ // header	  
	u_int32_t cmask = buff[idx+1]&0xFF;	  
	idx += 4;

	// Loop through channels
	for(unsigned int channel=0; channel<8; channel++){

	  int baseline = -1;
	  long int tbase = 0;
	  int bcount = 0;
	  unsigned int minval = 0x3fff, maxval=0;
	  
	  if(!((cmask>>channel)&1))
	    continue;
	  u_int32_t csize = buff[idx]&0x7FFFFF;
	  if(channel_finished[channel]>=5){
	    idx+=csize;
	    continue;
	  }
	  idx+=2;
	  
	  for(unsigned int i=0; i<csize-2; i++){
	    if(((buff[idx+i]&0xFFFF)==0) || (((buff[idx+i]>>16)&0xFFFF)==0))
	      continue;
	    tbase += buff[idx+i]&0xFFFF;
	    tbase += (buff[idx+i]>>16)&0xFFFF;
	    bcount+=2;
	    if((buff[idx+i]&0xFFFF)<minval)
	      minval = buff[idx+i]&0xFFFF;
	    if((buff[idx+i]&0xFFFF)>maxval)
	      maxval = buff[idx+i]&0xFFFF;
	    if(((buff[idx+i]>>16)&0xFFFF)<minval)
	      minval=(buff[idx+i]>>16)&0xFFFF;
	    if(((buff[idx+i]>>16)&0xFFFF)>maxval)
	      maxval=(buff[idx+i]>>16)&0xFFFF;	  
	  }
	  idx += csize-2;
	  if(abs(maxval-minval>50)){
	    std::cout<<"Signal in baseline, channel "<<channel
		     <<" min: "<<minval<<" max: "<<maxval<<std::endl;
	  }
	  else
	    baseline = int(float(tbase) / ((float(bcount))));
	  
	  if(baseline>=0){

	    // Time for the **magic**. We want to see how far we are off from nominal and
	    // adjust up and down accordingly. We will always adjust just a tiny bit
	    // less than we think we need to to avoid getting into some overshoot
	    // see-saw type loop where we never hit the target.
	    float absolute_unit = float(0xffff)/float(0x3fff);
	    int adjustment = .3*int(absolute_unit*((float(baseline)-float(target_value))));
	    //int adjustment = int(baseline)-int(target_value);
	    //std::cout<<dec<<"Adjustment: "<<adjustment<<" with threshold "<<adjustment_threshold<<std::endl;
	    //std::cout<<"Baseline: "<<baseline<<" DAC tihis channel: "<<dac_values[channel]<<std::endl;
	    if(abs(adjustment) < adjustment_threshold){
	      channel_finished[channel]++;
	      std::cout<<"Channel "<<channel<<" converging at step "<<channel_finished[channel]<<"/5"<<std::endl;
	    }
	    else{
	      channel_finished[channel]=0;
	      update_dac[channel] = true;
	      if(adjustment<0 && (u_int32_t(abs(adjustment)))>dac_values[channel])
		dac_values[channel]=0x0;
	      else if(adjustment>0 &&dac_values[channel]+adjustment>0xffff)
		dac_values[channel]=0xffff;
	      else {
		std::cout<<"Had channel "<<channel<<" at "<<dac_values[channel];
		dac_values[channel]+=(adjustment);
		std::cout<<" but now it's at "<<dac_values[channel]<<" (adjustment) BL: "<<baseline<<std::endl;
	      }
	    }
	  }
	} // End for loop through channels
	delete[] buff;
	break; // Need to break cause we just deleted buff
      } // End if found header
      else
	idx++;	
    } // End while through buff
    
  }// end iteration loop
  
  for(unsigned int x=0; x<channel_finished.size(); x++){
    if(channel_finished[x]<2){ // Be a little more lenient in case it's just starting to converge
      std::stringstream error;
      error<<"Baseline routine did not finish for channel "<<x<<" (and maybe others)."<<std::endl;
      fLog->Entry(error.str(), MongoLog::Error);
      return -1;
    }
  }

  
  end_values = dac_values;
  return 0;
}
*/
int V1724::LoadDAC(vector<u_int16_t>dac_values, vector<bool> &update_dac){
  // Loads DAC values into registers
  
  for(unsigned int x=0; x<dac_values.size(); x++){
    if(x>7 || update_dac[x]==false) // oops
      continue;

    // We updated, or at least tried to update
    //update_dac[x]=false;
    
    // Give the DAC time to be set if needed
    if(MonitorRegister((0x1088)+(0x100*x), 0x4, 100, 1000, 0) != true){
      stringstream errorstr;
      errorstr<<"Timed out waiting for channel "<<x<<" in DAC setting";
      fLog->Entry(errorstr.str(), MongoLog::Error);
      return -1;
    }

    // Now write channel DAC values
    if(WriteRegister((0x1098)+(0x100*x), dac_values[x])!=0){
      stringstream errorstr;
      errorstr<<"Failed writing DAC "<<hex<<dac_values[x]<<dec<<" in channel "<<x;
      fLog->Entry(errorstr.str(), MongoLog::Error);
      return -1;
    }

    // Give the DAC time to be set if needed
    if(MonitorRegister((0x1088)+(0x100*x), 0x4, 100, 1000, 0) != true){
      stringstream errorstr;
      errorstr<<"Timed out waiting for channel "<<x<<" after DAC setting";
      fLog->Entry(errorstr.str(), MongoLog::Error);
      return -1;
    }

  }
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
