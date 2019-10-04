#include "DAQController.hh"

// Status:
// 0-idle
// 1-arming
// 2-armed
// 3-running
// 4-error

DAQController::DAQController(MongoLog *log, std::string hostname){
  fLog=log;
  fHelper = new DAXHelpers();
  fOptions = NULL;
  fStatus = DAXHelpers::Idle;
  fReadLoop = false;
  fNProcessingThreads=8;
  fBufferLength = 0;
  fRawDataBuffer = NULL;
  fDatasize=0.;
  fHostname = hostname;
}

DAQController::~DAQController(){
  delete fHelper;
  if(fProcessingThreads.size()!=0)
    CloseProcessingThreads();
}

std::string DAQController::run_mode(){
  if(fOptions == NULL)
    return "None";
  try{
    return fOptions->GetString("name");
  }
  catch(const std::exception &e){
    return "None";
  }
}

int DAQController::InitializeElectronics(Options *options, std::vector<int>&keys,
					 std::map<int, std::vector<u_int16_t>>&written_dacs){

  End();
  
  fOptions = options;
  fNProcessingThreads = fOptions->GetNestedInt("processing_threads."+fHostname, 8);  
  fLog->Entry(MongoLog::Local, "Beginning electronics initialization with %i threads",
	      fNProcessingThreads);
  
  // Initialize digitizers
  fStatus = DAXHelpers::Arming;
  for(auto d : fOptions->GetBoards("V1724", fHostname)){
    fLog->Entry(MongoLog::Local, "Arming new digitizer %i", d.board);    

    V1724 *digi = new V1724(fLog, fOptions);
    if(digi->Init(d.link, d.crate, d.board, d.vme_address)==0){      
	fDigitizers[d.link].push_back(digi);
	if(std::find(keys.begin(), keys.end(), d.link) == keys.end()){	  
	  fLog->Entry(MongoLog::Local, "Defining a new optical link at %i", d.link);
	  keys.push_back(d.link);
	}    
	fLog->Entry(MongoLog::Debug, "Initialized digitizer %i", d.board);
    }
    else{
      fLog->Entry(MongoLog::Warning, "Failed to initialize digitizer %i", d.board);
      fStatus = DAXHelpers::Idle;
      return -1;
    }
  }

  fLog->Entry(MongoLog::Local, "Sleeping for two seconds");
  // For the sake of sanity and sleeping through the night, do not remove this statement.
  sleep(2); // <-- this one. Leave it here.
  // Seriously. This sleep statement is absolutely vital.
  fLog->Entry(MongoLog::Local, "That felt great, thanks.");
  
  // Load registers into digitizers  
  for( auto const& link : fDigitizers )    {
    for(auto digi : link.second){

      fLog->Entry(MongoLog::Local, "Beginning specific init for board %i", digi->bid());
      // Load initial registers
      int write_success = 0;
      write_success += digi->WriteRegister(0xEF24, 0x1);
      write_success += digi->WriteRegister(0xEF00, 0x30);
      if(write_success!=0){
	fLog->Entry(MongoLog::Error, "Digitizer %i unable to load pre-registers", digi->bid());
	fStatus = DAXHelpers::Idle;
	return -1;
      }
      
      // Load DAC. n.b.: if you set the DAC value in your ini file you'll overwrite
      // the fancy stuff done here!
      vector<u_int16_t>dac_values(8, 0x0);

      // Multiple options here
      std::string BL_MODE = fOptions->GetString("baseline_dac_mode", "fixed");
      int success = 0;
      if(BL_MODE == "fit"){      
	int nominal_dac = fOptions->GetInt("baseline_value", 16000);
	fLog->Entry(MongoLog::Local,
		    "You're fitting baselines to digi %i. Starting by getting start values",
		    digi->bid());

	// Set starting values to most recent run
	fLog->GetDACValues(digi->bid(), -1, dac_values);

	// Try up to five times since sometimes will not converge. If the function
	// returns -2 it means it crashed hard so don't bother trying again.
	int tries=0;
	do{
	  fLog->Entry(MongoLog::Local, "Going into DAC routine. Try: %i", tries+1);
	  success = digi->ConfigureBaselines(dac_values, nominal_dac, 500);
	  tries++;
	} while(tries<5 && success==-1);
      }
      else if(BL_MODE == "cached"){
	int rrun = fOptions->GetInt("baseline_reference_run", -1);
	fLog->Entry(MongoLog::Local, "You're loading cached baselines for digi: %i", digi->bid());
	if(rrun == -1 || fLog->GetDACValues(digi->bid(), rrun, dac_values) != 0){
	  fLog->Entry(MongoLog::Warning, "Asked for cached baselines but can't find baseline_reference_run. Fallback to fixed");
	  BL_MODE = "fixed"; // fallback in case no run set
	}
      }
      else if(BL_MODE != "fixed"){
	fLog->Entry(MongoLog::Warning, "Received unknown baseline mode. Fallback to fixed");
	BL_MODE = "fixed";
      }
      if(BL_MODE == "fixed"){
	int BLVal = fOptions->GetInt("baseline_fixed_value", 4000);
	fLog->Entry(MongoLog::Local, "Loading fixed baselines at value 0x%04x for digi %i",
		    BLVal, digi->bid());
	for(unsigned int x=0;x<dac_values.size();x++)
	  dac_values[x] = BLVal;
      }
	
      //int success = 0;
      std::cout<<"Baselines finished for digi "<<digi->bid()<<std::endl;
      if(success==-2){
	fLog->Entry(MongoLog::Warning, "Baselines failed with digi error");
	fStatus = DAXHelpers::Error;
	return -1;	
      }
      else if(success!=0){
	fLog->Entry(MongoLog::Warning, "Baselines failed with timeout");
	fStatus	= DAXHelpers::Idle;
        return -1;
      }
      
      fLog->Entry(MongoLog::Local, "Digi %i survived baseline mode. Going into register setting",
		  digi->bid());

      for(auto regi : fOptions->GetRegisters(digi->bid())){
	unsigned int reg = fHelper->StringToHex(regi.reg);
	unsigned int val = fHelper->StringToHex(regi.val);
	success+=digi->WriteRegister(reg, val);	
      }
      fLog->Entry(MongoLog::Local, "User registers finished for digi %i. Loading DAC.",
                  digi->bid());


      // Load the baselines you just configured
      vector<bool> update_dac(8, true);
      success += digi->LoadDAC(dac_values, update_dac);
      written_dacs[digi->bid()] = dac_values;
      std::cout<<"Configuration finished for digi "<<digi->bid()<<std::endl;

      fLog->Entry(MongoLog::Local,
		  "DAC finished for %i. Assuming not directly followed by an error, that's a wrap.",
                  digi->bid());
      if(success!=0){
	//LOG
	fStatus = DAXHelpers::Idle;
	fLog->Entry(MongoLog::Warning, "Failed to configure digitizers.");
	return -1;
      }
      
    }
  }

  for(auto const& link : fDigitizers ) {    
    for(auto digi : link.second){      
      if(fOptions->GetInt("run_start", 0) == 1)
	digi->WriteRegister(0x8100, 0x5);
      else
	digi->WriteRegister(0x8100, 0x0);
    }
  }
  sleep(1);
  fStatus = DAXHelpers::Armed;

  fLog->Entry(MongoLog::Local, "Arm command finished, returning to main loop");


  return 0;
}

int DAQController::Start(){
  if(fOptions->GetInt("run_start", 0) == 0){
    for( auto const& link : fDigitizers ){      
      for(auto digi : link.second){

	// Ensure digitizer is ready to start
	if(digi->MonitorRegister(0x8104, 0x100, 1000, 1000)!=true){
	  fLog->Entry(MongoLog::Warning, "Digitizer not ready to start after sw command sent");
	  return -1;
	}

	// Send start command
	digi->WriteRegister(0x8100, 0x4);

	// Ensure digitizer is started
	if(digi->MonitorRegister(0x8104, 0x4, 1000, 1000) != true){
	  fLog->Entry(MongoLog::Warning,
		      "Timed out waiting for acquisition to start after SW start sent");
	  return -1;
	}
      }
    }
  }
  fStatus = DAXHelpers::Running;
  return 0;
}

int DAQController::Stop(){

  std::cout<<"Deactivating boards"<<std::endl;
  for( auto const& link : fDigitizers ){      
    for(auto digi : link.second){
      
      digi->WriteRegister(0x8100, 0x0);

      // Ensure digitizer is stopped 
      if(digi->MonitorRegister(0x8104, 0x4, 1000, 1000, 0x0) != true){
	fLog->Entry(MongoLog::Warning,
		    "Timed out waiting for acquisition to stop after SW stop sent");
          return -1;
      }
    }
  }
  fLog->Entry(MongoLog::Debug, "Stopped digitizers");

  fReadLoop = false; // at some point.
  fStatus = DAXHelpers::Idle;
  return 0;
}
void DAQController::End(){
  Stop();
  std::cout<<"Closing Processing Threads"<<std::endl;
  CloseProcessingThreads();
  std::cout<<"Closing Digitizers"<<std::endl;
  for( auto const& link : fDigitizers ){    
    for(auto digi : link.second){
      digi->End();
      delete digi;
    }
  } 
  fDigitizers.clear();
  fStatus = DAXHelpers::Idle;

  if(fRawDataBuffer != NULL){
    fLog->Entry(MongoLog::Warning, "Deleting uncleard buffer of size %i",
		fRawDataBuffer->size());	       
    for(unsigned int i=0; i<fRawDataBuffer->size(); i++){
      delete[] (*fRawDataBuffer)[i].buff;
    }
    delete fRawDataBuffer;
    fRawDataBuffer = NULL;
  }

  std::cout<<"Finished end"<<std::endl;
}

void* DAQController::ReadThreadWrapper(void* data, int link){
  DAQController *dc = static_cast<DAQController*>(data);
  dc->ReadData(link);
  return dc;
}  

void DAQController::ReadData(int link){
  fReadLoop = true;
  // CloseProcessingThreads();
  // OpenProcessingThreads();
  
  // Raw data buffer should be NULL. If not then maybe it was not cleared since last time
  if(fRawDataBuffer != NULL){
    fLog->Entry(MongoLog::Debug, "Raw data buffer being brute force cleared.");	       
    for(unsigned int x=0;x<fRawDataBuffer->size(); x++){
      delete[] (*fRawDataBuffer)[x].buff;
    }
    delete fRawDataBuffer;
    fBufferLength=0;
    fRawDataBuffer = NULL;
  }
  
  u_int32_t lastRead = 0; // bytes read in last cycle. make sure we clear digitizers at run stop
  long int readcycler = 0;
  while(fReadLoop){// || lastRead > 0){
    //if(fReadLoop==false)
    //  std::cout<<lastRead<<std::endl;
    //lastRead = 0;
    
    vector<data_packet> local_buffer;
    for(unsigned int x=0; x<fDigitizers[link].size(); x++){

      // Every 1k reads check board status
      if(readcycler%10000==0){
	readcycler=0;
	u_int32_t data = fDigitizers[link][x]->ReadRegister(0x8104);
	std::cout<<"Board "<<fDigitizers[link][x]->bid()<<" has status "<<hex<<data<<dec<<std::endl;
      }
      data_packet d;
      d.buff=NULL;
      d.size=0;
      d.bid = fDigitizers[link][x]->bid();
      d.size = fDigitizers[link][x]->ReadMBLT(d.buff);

      // Here's the fancy part. We gotta grab the header of the first
      // event in the buffer and get the clock reset counter from the
      // board. This gets shipped off with the buffer.
      u_int32_t idx=0;
      while(idx < d.size/sizeof(u_int32_t)){
	if(d.buff[idx]>>20==0xA00){
	  d.header_time = d.buff[idx+3]&0x7FFFFFFF;
	  d.clock_counter = fDigitizers[link][x]->GetClockCounter(d.header_time);
	  break;
	}
	idx++;
      }
      
      lastRead += d.size;
      
      if(d.size<0){
	//LOG ERROR
	if(d.buff!=NULL)
	  delete[] d.buff;
	break;
      }
      if(d.size>0){
	fDatasize += d.size;
	if(fDataPerDigi.find(d.bid) == fDataPerDigi.end())
	  fDataPerDigi[d.bid] = 0;
	fDataPerDigi[d.bid] += d.size;
	local_buffer.push_back(d);
      }
    }
    if(local_buffer.size()!=0)
      AppendData(local_buffer);
    local_buffer.clear();
    readcycler++;
  }

}

std::map<int, u_int64_t> DAQController::GetDataPerDigi(){
  // Return a map of data transferred per digitizer since last update
  // and clear the private map
  std::map retmap = fDataPerDigi;
  for(auto const &kPair : fDataPerDigi)
    fDataPerDigi[kPair.first] = 0;
  return retmap;
}

void DAQController::AppendData(vector<data_packet> &d){
  // Blocks!
  fBufferMutex.lock();
  if(fRawDataBuffer==NULL)
    fRawDataBuffer = new std::vector<data_packet>();
  fRawDataBuffer->insert( fRawDataBuffer->end(), d.begin(), d.end() );
  u_int64_t bl = 0;
  for(unsigned int x=0; x<fRawDataBuffer->size(); x++){
    bl += (*fRawDataBuffer)[x].size;
  }
  fBufferLength = bl; 
  fBufferMutex.unlock();  
}

int DAQController::GetData(std::vector <data_packet> *&retVec){
  // Check once, is it worth locking mutex?
  retVec=NULL;
  if(fBufferLength==0)
    return 0;
  if(!fBufferMutex.try_lock())
    return 0;

  int ret = 0;
  // Check again, is there still data?
  if(fRawDataBuffer != NULL && fRawDataBuffer->size()>0){

    // Pass ownership to calling function
    retVec = fRawDataBuffer;
    fRawDataBuffer = NULL;

    ret = retVec->size();
    fBufferLength = 0;
  }
  fBufferMutex.unlock();
  return ret;
}
  

void* DAQController::ProcessingThreadWrapper(void* data){
  //MongoInserter *mi = static_cast<MongoInserter*>(data);
  StraxInserter *mi = static_cast<StraxInserter*>(data);
  mi->ReadAndInsertData();
  return data;
}

bool DAQController::CheckErrors(){

  // This checks for errors from the threads by checking the
  // error flag in each object. It's appropriate to poll this
  // on the order of ~second(s) and initialize a STOP in case
  // the function returns "true"

  for(unsigned int i=0; i<fProcessingThreads.size(); i++){
    if(fProcessingThreads[i].inserter->CheckError()){
      fLog->Entry(MongoLog::Error, "Error found in processing thread.");
      fStatus=DAXHelpers::Error;
      return true;
    }
  }
  return false;
}

void DAQController::OpenProcessingThreads(){

  for(int i=0; i<fNProcessingThreads; i++){
    processingThread p;
    //p.inserter = new MongoInserter();
    p.inserter = new StraxInserter();
    p.inserter->Initialize(fOptions, fLog, this, fHostname);
    p.pthread = new std::thread(ProcessingThreadWrapper,
			       static_cast<void*>(p.inserter));
    fProcessingThreads.push_back(p);
  }

}

void DAQController::CloseProcessingThreads(){

  for(unsigned int i=0; i<fProcessingThreads.size(); i++){
    fProcessingThreads[i].inserter->Close();
    fProcessingThreads[i].pthread->join();
        
    delete fProcessingThreads[i].pthread;
    delete fProcessingThreads[i].inserter;
  }
  fProcessingThreads.clear();
}
