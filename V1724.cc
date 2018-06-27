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
    if(timestamp >= 15e8 && seen_under_5)
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
    return clock_counter;
  }  
}

int V1724::WriteRegister(unsigned int reg, unsigned int value){
  //std::cout<<"Writing reg:val: "<<hex<<reg<<":"<<value<<dec<<std::endl;
  if(CAENVME_WriteCycle(fBoardHandle,fBaseAddress+reg,
			&value,cvA32_U_DATA,cvD32) != cvSuccess){
    std::stringstream err;
    err<<"Failed to write register 0x"<<hex<<reg<<dec<<" to board "<<fBID<<
      " with value "<<hex<<value<<dec<<" board handle "<<fBoardHandle<<endl;
    fLog->Entry(err.str(), MongoLog::Warning);
    return -1;
  }
  return 0;
}

unsigned int V1724::ReadRegister(unsigned int reg){
  unsigned int temp;
  if(CAENVME_ReadCycle(fBoardHandle, fBaseAddress+reg, &temp,
		       cvA32_U_DATA, cvD32) != cvSuccess){
    std::stringstream err;
    err<<"Failed to read register 0x"<<hex<<reg<<dec<<" on board "<<fBID<<endl;
    fLog->Entry(err.str(), MongoLog::Warning);
    return -1;
  }
  return temp;
}

u_int32_t V1724::ReadMBLT(unsigned int *&buffer){
  // Initialize
  unsigned int blt_bytes=0;
  int nb=0,ret=-5;
  unsigned int BLT_SIZE=8*8388608; // 8MB buffer size
  u_int32_t *tempBuffer = new u_int32_t[BLT_SIZE/4];

  int count = 0;
  do{
    ret = CAENVME_FIFOBLTReadCycle(fBoardHandle, fBaseAddress,
				   ((unsigned char*)tempBuffer)+blt_bytes,
				   BLT_SIZE, cvA32_U_BLT, cvD32, &nb);
    if( (ret != cvSuccess) && (ret != cvBusError) ){
      stringstream err;
      err<<"Read error in board "<<fBID<<" after "<<count<<" reads.";
      fLog->Entry(err.str(), MongoLog::Error);
      delete[] tempBuffer;
      return 0;
    }

    count++;
    blt_bytes+=nb;

    if(blt_bytes>BLT_SIZE){
      stringstream err;
      err<<"You managed to transfer more data than fits on board."<<
	"Transferred: "<<blt_bytes<<" bytes, Buffer: "<<BLT_SIZE<<" bytes.";
      fLog->Entry(err.str(), MongoLog::Error);
      
      delete[] tempBuffer;
      return 0;
    }
  }while(ret != cvBusError);


  // Now, unfortunately we need to make one copy of the data here or else our memory
  // usage explodes. We declare above a buffer of 8MB, which is the maximum capacity
  // of the board in case every channel is 100% saturated (the practical largest
  // capacity is certainly smaller depending on settings). But if we just keep reserving
  // 8MB blocks and filling 500kB with actual data, we're gonna run out of memory.
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

int V1724::ConfigureBaselines(int nominal_value,
			      int ntries,
			      vector <unsigned int> start_values,
			      vector <unsigned int> &end_values){

  // Baseline configuration routine. The V1724 has a DAC offset setting that
  // biases the ADC, allowing you to effectively move the 'zero level' of the
  // acquisition up or down. But keep in mind this is a bit limited. It's not
  // designed to correct large offsets O(~volts) (i.e. your detector ground very far
  // from ADC ground) but rather to move the baseline to the location best
  // suited to your acquisition (positive, negative, bipolar logic)

  // n.b. hard-coding guess to 16000 for testing
  u_int32_t target_value = 16000;
  int max_deviation = 5;

  // We can adjust a DAC offset register, which is 0xffff in range, is inversely
  // proportional to the baseline position, and has ~5% overshoot on either end.
  // So we use this information to get starting values:
  u_int32_t starting_value = u_int32_t( (0x3fff-target_value)*((0.9*0xffff)/0x3fff)) + 3277;
  int nChannels = 8;
  vector<u_int32_t> dac_values(nChannels, starting_value);
  vector<bool> channel_finished(nChannels, false);
  
  // Now we need to load a simple configuration to the board in order to read
  // some data. try/catch cause CAENVMElib fails poorly (segfault) sometimes
  // in case the hardware has an issue.
  write_success = 0;
  try{    
    write_success += WriteRegister(0x8000, 0x10);      // Channel configuration
    write_success += WriteRegister(0x8080, 0x800000);  // DPP
    write_success += WriteRegister(0x8100, 0x0);       // Acq. control
    write_success += WriteRegister(0x810C, 0x80000000);// Trigger source
    write_success += WriteRegister(0xEF24, 0x1);       // Global reset
    write_success += WriteRegister(0xEF1C, 0x1);       // BERR
    write_success += WriteRegister(0xEF00, 0x10);      // Channel memory
    write_success += WriteRegister(0x8120, 0xFF);      // Channel mask
  }
  catch(const std::exception &e){
    std::stringstream error;
    error<<"Digitizer "<<fBID<<" CAEN fault during initial register adjustment in baseline routine";
    fLog->Entry(error.str(), MongoLog::Error);
    return -1;
  }

  // Slightly more palatable error, at least CAENVMElib is recognizing a failure
  // and not just seg faulting
  if(write_success!=0){
    std::stringstream error;
    error<<"Digitizer "<<fBID<<" unable to load registers for baselines.";
    fLog->Entry(error.str(), MongoLog::Error);
    return -1;
  }

  // Now we'll iterate for a while. It should be pretty quick since starting values
  // should be quite close to true.
  int currentIteration = 0;
  int maxIterations = 100;
  while(currentIteration < maxIterations){
    currentIteration++;
    bool breakout = true;
    
    for(unsigned int channel=0; channel<nChannels; channel++){
      if(channel_finished[channel])
	continue;
      breakout = false;

      // Load DAC for this channel
      if(LoadDAC(channel, dac_values[channel])!=0){
	std::stringstream error;
	error<<"Digitizer "<<fBID<<" channel "<<channel<<" failed to load DAC in baseline routine.";
	fLog->Entry(error.str(), MongoLog::Error);
	return -1;
      }

      // Tell board to read out this channel only
      unsigned short channel_mask = 1 << channel;
      if(WriteRegister(0x8120, channel_mask)!=0){
	stringstream error;
	error<<"Digitizer "<<fBID<<" channel "<<channel<<" failed to set channel mask in baseline.";
	fLog->Entry(error.str(), MongoLog::Error);
	return -1;
      }

      // Trigger the board with software trigger
      WriteRegister(CBV1724_AcquisitionControlReg,0x24);
      WriteRegister(CBV1724_SoftwareTriggerReg,0x1);
      usleep(50); // paranoia. Like, you need some time to acquire right?
      WriteRegister(CBV1724_AcquisitionControlReg,0x0);

      
      // Sample Samples
      // Good? then finish
      // Else twiddle DAC offset
      // Repeat
    }

    if(breakout)
      break;
  }
  
    
  // 0x1n88 channel n DAC busy 0b001 (1 - yes)
  // 0x1n98, 0x8098 DAC value 0xffff
  cout<<"Not implemented"<<endl;
  return 0;
}
int V1724::End(){
  if(fBoardHandle>=0)
    CAENVME_End(fBoardHandle);
  fBoardHandle=fLink=fCrate=fBID=-1;
  fBaseAddress=0;
  return 0;
}

