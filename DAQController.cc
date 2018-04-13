#include "DAQController.hh"

// Status:
// 0-idle
// 1-arming
// 2-armed
// 3-running
// 4-error

DAQController::DAQController(){
  fHelper = new DAXHelpers();
  fOptions = NULL;
  fStatus = 0;
  fReadLoop = false;
  fNProcessingThreads=8;
  fBufferLength = 0;
  fRunStartController = NULL;
  fRawDataBuffer = NULL;
}
DAQController::~DAQController(){
  delete fHelper;
  if(fOptions != NULL)
    delete fOptions;
  if(fProcessingThreads.size()!=0)
    CloseProcessingThreads();
}

int DAQController::InitializeElectronics(string opts){

  if(fOptions != NULL)
    delete fOptions;
  fOptions = new Options(opts);
    
  fStatus = 1;
  for(auto d : fOptions->GetBoards("V1724")){
    
    V1724 *digi = new V1724();
    if(digi->Init(d.link, d.crate, d.board, d.vme_address)==0){      
      fDigitizers.push_back(digi);
      std::cout<<"Initialized digitizer "<<d.board<<std::endl;
    }
    else{
      std::cout<<"Failed to initialize digitizer "<<d.board<<std::endl;
      fStatus = 0;
      return -1;
    }
  }
  
  for(auto digi : fDigitizers){
    int success=0;
    for(auto regi : fOptions->GetRegisters(digi->bid())){
      unsigned int reg = fHelper->StringToHex(regi.reg);
      unsigned int val = fHelper->StringToHex(regi.val);
      success+=digi->WriteRegister(reg, val);
    }
    if(success!=0){
      //LOG
      fStatus = 0;
      std::cout<<"Failed to write registers."<<std::endl;
      return -1;
    }
  }
  for(unsigned int x=0;x<fDigitizers.size();x++)
    fDigitizers[x]->WriteRegister(0x8100, 0x0);
  fStatus = 2;
  return 0;
}

void DAQController::Start(){
  if(fRunStartController==NULL){
    for(unsigned int x=0;x<fDigitizers.size(); x++)
      fDigitizers[x]->WriteRegister(0x8100, 0x4);
  }
  fStatus = 3;
  return;
}

void DAQController::Stop(){

  if(fRunStartController==NULL){
    for(unsigned int x=0;x<fDigitizers.size();x++)
      fDigitizers[x]->WriteRegister(0x8100, 0x0);
  }
  fReadLoop = false; // at some point.
  CloseProcessingThreads();
  fStatus = 0;
  End(); // Leave option open in future to separate stop/end
  return;
}
void DAQController::End(){
  for(unsigned int x=0; x<fDigitizers.size(); x++){
    fDigitizers[x]->End();
    delete fDigitizers[x];
  }
  fDigitizers.clear();
  fStatus = 0;

  if(fRawDataBuffer != NULL){
    std::cerr<<"Caution: deleting uncleared data buffer"<<std::endl;
    for(unsigned int i=0; i<fRawDataBuffer->size(); i++){
      delete[] (*fRawDataBuffer)[i].buff;
    }
    delete fRawDataBuffer;
    fRawDataBuffer = NULL;
  }
}

void* DAQController::ReadThreadWrapper(void* data){
  DAQController *dc = static_cast<DAQController*>(data);
  dc->ReadData();
  return dc;
}  

void DAQController::ReadData(){
  fReadLoop = true;
  CloseProcessingThreads();
  OpenProcessingThreads();
  
  // Raw data buffer should be NULL. If not then maybe it was not cleared since last time
  if(fRawDataBuffer != NULL){
    std::cout<<"Raw data buffer being brute force cleared."<<std::endl;
    for(unsigned int x=0;x<fRawDataBuffer->size(); x++){
      delete[] (*fRawDataBuffer)[x].buff;
    }
    delete fRawDataBuffer;
    fRawDataBuffer = NULL;
  }
  
  u_int32_t lastRead = 0; // bytes read in last cycle. make sure we clear digitizers at run stop
  while(fReadLoop || lastRead > 0){
    lastRead = 0;
    
    vector<data_packet> local_buffer;
    for(unsigned int x=0; x<fDigitizers.size(); x++){
      data_packet d;
      d.buff=NULL;
      d.size=0;
      d.bid = fDigitizers[x]->bid();
      d.size = fDigitizers[x]->ReadMBLT(d.buff);      
      lastRead += d.size;
      
      if(d.size<0){
	//LOG ERROR
	if(d.buff!=NULL)
	  delete[] d.buff;
	break;
      }
      if(d.size!=0)
	local_buffer.push_back(d);
    }
    if(local_buffer.size()!=0)
      AppendData(local_buffer);
    local_buffer.clear();
  }

}

void DAQController::AppendData(vector<data_packet> d){
  // Blocks!
  fBufferMutex.lock();
  if(fRawDataBuffer==NULL)
    fRawDataBuffer = new std::vector<data_packet>();
  fRawDataBuffer->insert( fRawDataBuffer->end(), d.begin(), d.end() );
  fBufferLength = fRawDataBuffer->size();
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
  MongoInserter *mi = static_cast<MongoInserter*>(data);
  mi->ReadAndInsertData();
  return data;
}


void DAQController::OpenProcessingThreads(){

  for(int i=0; i<fNProcessingThreads; i++){
    processingThread p;
    p.inserter = new MongoInserter();
    p.inserter->Initialize(fOptions, this);
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
