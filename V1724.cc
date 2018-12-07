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

  // Oh yes I will
  // MINESWEEPER
  stringstream command;
  command<<"(cd /home/xedaq/minesweeper && echo `./minesweeper -l "<<
    link<<" -c "<<crate<<"`)";
  cout<<"Sending command: "<<command.str()<<endl;
  int retsys = system(command.str().c_str());
  cout<<"Returned: "<<retsys<<endl;
  usleep(1000);
  //
	  
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
  std::cout<<hex<<"Wrote register "<<reg<<" with value "<<value<<" for board "<<dec<<fBID<<std::endl;
  usleep(100); // don't ask
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
  return temp;
}

u_int32_t V1724::ReadMBLT(unsigned int *&buffer){
  // Initialize
  unsigned int blt_bytes=0;
  int nb=0,ret=-5;
  // The best-equipped V1724E has 4MS/channel memory = 8 MB/channel
  // the other, V1724G, has 512 MS/channel = 1MB/channel
  unsigned int BLT_SIZE=8388608; //8*8388608; // 8MB buffer size
  u_int32_t *tempBuffer = new u_int32_t[BLT_SIZE*2];

  int count = 0;
  do{
    try{
      ret = CAENVME_FIFOBLTReadCycle(fBoardHandle, fBaseAddress,
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
  u_int32_t starting_value = 0x1000; //u_int32_t( (0x3fff-target_value)*((0.9*0xffff)/0x3fff)) + 3277;
  int nChannels = 8;
  vector<u_int16_t> dac_values(nChannels, starting_value);
  vector<bool> channel_finished(nChannels, false);
  vector<bool> update_dac(nChannels, true);
  
  // Now we need to load a simple configuration to the board in order to read
  // some data. try/catch cause CAENVMElib fails poorly (segfault) sometimes
  // in case the hardware has an issue.
  int write_success = 0;
  try{
    write_success += WriteRegister(0xEF24, 0x1);       // Global reset
    write_success += WriteRegister(0xEF1C, 0x1);       // BERR 
    write_success += WriteRegister(0xEF00, 0x10);      // Channel memory
    write_success += WriteRegister(0x8120, 0xFF);      // Channel mask
    write_success += WriteRegister(0x8020, 0x1F4);     // Buffer size

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
    for(int channel=0; channel<nChannels; channel++){
      if(channel_finished[channel])
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
    
    u_int32_t *buff = NULL;
    u_int32_t size = 0;
    
    WriteRegister(0xEF28, 0x1);       // Software clear any old data
    usleep(1000);

    // Make sure we're ready for acquisition
    u_int32_t data = 0;
    int readycount = 0;
    while(!data&0x100 && readycount < 1000){
      usleep(1000);
      data = ReadRegister(0x8104);
      readycount++;
    }
    if(readycount>=1000){
      fLog->Entry("Timed out waiting for board to be ready in baselines", MongoLog::Warning);
      return -1;
    }
    
    WriteRegister(0x8100,0x4);//x24?   // Acq control reg
    usleep(1000);
    WriteRegister(0x8108,0x1);    // Software trig reg      
    usleep(1000);
    
    int readcount = 0;
    while(size == 0 && readcount < 1000){	
      size = ReadMBLT(buff);
      usleep(10);
      readcount++;
      if(size>0 && size<=16){
	std::cout<<"Delete undersized buffer ("<<size<<")"<<std::endl;
	size = 0;
	delete[] buff;
	buff = NULL;
      }	
    }
    if(readcount >= 1000){
      WriteRegister(0x8100, 0x0);
      continue;
    }
    WriteRegister(0x8100, 0x0);
    
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
	  if(channel_finished[channel]){
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
	    int adjustment = int(absolute_unit*((float(baseline)-float(target_value))));
	    //int adjustment = int(baseline)-int(target_value);
	    //std::cout<<dec<<"Adjustment: "<<adjustment<<" with threshold "<<adjustment_threshold<<std::endl;
	    //std::cout<<"Baseline: "<<baseline<<" DAC tihis channel: "<<dac_values[channel]<<std::endl;
	    if(abs(adjustment) < adjustment_threshold){
	      channel_finished[channel]=true;
	      std::cout<<"Channel "<<channel<<" finished"<<std::endl;
	    }
	    else{
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
    if(channel_finished[x]!=true){
      std::stringstream error;
      error<<"Baseline routine did not finish for channel "<<x<<" (and maybe others)."<<std::endl;
      fLog->Entry(error.str(), MongoLog::Error);
      return -1;
    }
  }

  
  end_values = dac_values;
  return 0;
}

int V1724::LoadDAC(vector<u_int16_t>dac_values, vector<bool> &update_dac){
  // Loads DAC values into registers
  
  for(unsigned int x=0; x<dac_values.size(); x++){
    if(x>7 || update_dac[x]==false) // oops
      continue;

    // We updated, or at least tried to update
    update_dac[x]=false;
    
    // Define a counter to give the DAC time to be set if needed
    int counter = 0; 
    while(counter < 100){
      u_int32_t data = 0x4;
      data = ReadRegister((0x1088)+(0x100*x)); // DAC ready register
      if(data&0x4){
	usleep(1000);
	counter++;
	continue;
      }
      break;
    }
    if(counter >= 100){
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

    // Wait for it to kick in
    counter = 0;
    while(counter < 100){
      u_int32_t data = 0x4;
      data = ReadRegister((0x1088)+(0x100*x));
      if(data&0x4){
	usleep(1000);
	counter++;
	continue;
      }
      break;
    }
    if(counter >= 100){
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

