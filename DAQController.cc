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
  fNProcessingThreads=16;
  fBufferLength = 0;
  fRawDataBuffer = NULL;
  fDatasize=0.;
  fHostname = hostname;
  fStraxHandler = new StraxFileHandler(log);
}

DAQController::~DAQController(){
  delete fHelper;
  if(fProcessingThreads.size()!=0)
    CloseProcessingThreads();
  delete fStraxHandler;
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

int DAQController::InitializeElectronics(Options *options, std::vector<int>&keys){

  End();
  
  fOptions = options;
  std::cout<<"Initializing digitizers"<<std::endl;
  
  // Initialize digitizers
  fStatus = DAXHelpers::Arming;
  for(auto d : fOptions->GetBoards("V1724", fHostname)){
    std::cout<<"New digitizer "<<d.board<<std::endl;
    V1724 *digi = new V1724(fLog);
    if(digi->Init(d.link, d.crate, d.board, d.vme_address)==0){      
	fDigitizers[d.link].push_back(digi);
	if(std::find(keys.begin(), keys.end(), d.link) == keys.end()){
	  std::cout<<"Defining new optical link "<<d.link<<std::endl;
	  keys.push_back(d.link);
	}    
	std::stringstream mess;
	mess<<"Initialized digitizer "<<d.board;
	fLog->Entry(mess.str(), MongoLog::Debug);
    }
    else{
      std::stringstream err;
      err<<"Failed to initialize digitizer "<<d.board;
      fLog->Entry(err.str(), MongoLog::Warning);
      fStatus = DAXHelpers::Idle;
      return -1;
    }
  }
  
  // Load registers into digitizers
  std::cout<<"Loading registers"<<std::endl;
  for( auto const& link : fDigitizers )    {
    for(auto digi : link.second){

      // Load DAC. n.b.: if you set the DAC value in your ini file you'll overwrite
      // the fancy stuff done here!
      vector<u_int16_t>dac_values(8, 0x1000);
      int nominal_dac = fOptions->GetInt("baseline_value", 16000);
      std::cout<<"Setting baselines for digi "<<digi->bid()<<std::endl;
      int success = digi->ConfigureBaselines(dac_values, nominal_dac, 500);
      //int success = 0;
      std::cout<<"Baselines finished for digi "<<digi->bid()<<std::endl;
      if(success!=0){
	fLog->Entry("Baselines failed with digi error", MongoLog::Warning);
	return -1;
      }

      std::cout<<"Writing user registers for digi "<<digi->bid()<<std::endl;
      for(auto regi : fOptions->GetRegisters(digi->bid())){
	unsigned int reg = fHelper->StringToHex(regi.reg);
	unsigned int val = fHelper->StringToHex(regi.val);
	success+=digi->WriteRegister(reg, val);
      }
      std::cout<<"User registers finished for digi "<<digi->bid()<<", loading DAC"<<std::endl;

      // Load the baselines you just configured
      vector<bool> update_dac(8, true);
      success += digi->LoadDAC(dac_values, update_dac);
      std::cout<<"Configuration finished for digi "<<digi->bid()<<std::endl;
      
      if(success!=0){
	//LOG
	fStatus = DAXHelpers::Idle;
	fLog->Entry("Failed to write registers.", MongoLog::Warning);
      return -1;
      }
    }
  }

  // Look at this later! This initializes all boards to SW controlled
  // and inactive. Will need option for HW control.
  std::cout<<"Setting register off"<<std::endl;
  for( auto const& link : fDigitizers ) {
    for(auto digi : link.second){
      digi->WriteRegister(0x8100, 0x5);
    }
  }
  fStatus = DAXHelpers::Armed;

  std::cout<<"Printing to string"<<std::endl;
  std::cout<<fOptions->ExportToString()<<std::endl;

  // Last thing we need to do is get our strax writer ready.
  std::string strax_output_path = fOptions->GetString("strax_output_path", "./out");
  std::string run_name = fOptions->GetString("run_identifier", "run");
  u_int32_t full_fragment_size = (fOptions->GetInt("strax_header_size", 31) +
				  fOptions->GetInt("strax_fragment_length", 220));
  std::cout<<"Initializing strax with "<<full_fragment_size<<" fragment size"<<std::endl;
  fStraxHandler->Initialize(strax_output_path, run_name, full_fragment_size, fHostname);

  return 0;
}

void DAQController::Start(){
  if(fOptions->GetInt("run_start", 0) == 0){
    for( auto const& link : fDigitizers ){      
      for(auto digi : link.second){
	digi->WriteRegister(0x8100, 0x4);
      }
    }
  }
  fStatus = DAXHelpers::Running;
  return;
}

void DAQController::Stop(){

  std::cout<<"Deactivating boards"<<std::endl;
  for( auto const& link : fDigitizers ){      
    for(auto digi : link.second){
      digi->WriteRegister(0x8100, 0x0);
    }
  }      
  fLog->Entry("Stopped digitizers", MongoLog::Debug);

  fReadLoop = false; // at some point.
  fStatus = DAXHelpers::Idle;
  return;
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
    std::stringstream warn_entry;
    warn_entry<<"Deleting uncleared data buffer of size "<<
      fRawDataBuffer->size();
    fLog->Entry(warn_entry.str(),
		MongoLog::Warning);
    for(unsigned int i=0; i<fRawDataBuffer->size(); i++){
      delete[] (*fRawDataBuffer)[i].buff;
    }
    delete fRawDataBuffer;
    fRawDataBuffer = NULL;
  }
  std::cout<<"Closing strax output"<<std::endl;
  // Assume everything is read out so we can close strax
  fStraxHandler->End();
  std::cout<<"Finished end"<<std::endl;
}

void* DAQController::ReadThreadWrapper(void* data, int link){
  DAQController *dc = static_cast<DAQController*>(data);
  dc->ReadData(link);
  return dc;
}  

void DAQController::ReadData(int link){
  fReadLoop = true;
  CloseProcessingThreads();
  OpenProcessingThreads();
  
  // Raw data buffer should be NULL. If not then maybe it was not cleared since last time
  if(fRawDataBuffer != NULL){
    fLog->Entry("Raw data buffer being brute force cleared.",
		MongoLog::Debug);
    for(unsigned int x=0;x<fRawDataBuffer->size(); x++){
      delete[] (*fRawDataBuffer)[x].buff;
    }
    delete fRawDataBuffer;
    fBufferLength=0;
    fRawDataBuffer = NULL;
  }
  
  u_int32_t lastRead = 0; // bytes read in last cycle. make sure we clear digitizers at run stop
  while(fReadLoop){// || lastRead > 0){
    //if(fReadLoop==false)
    //  std::cout<<lastRead<<std::endl;
    //lastRead = 0;
    
    vector<data_packet> local_buffer;
    for(unsigned int x=0; x<fDigitizers[link].size(); x++){
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
	local_buffer.push_back(d);
      }
    }
    if(local_buffer.size()!=0)
      AppendData(local_buffer);
    local_buffer.clear();
  }

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
      fLog->Entry("Error found in processing thread.", MongoLog::Error);
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
    p.inserter->Initialize(fOptions, fLog, fStraxHandler, this);
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
