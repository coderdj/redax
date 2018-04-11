#include "DAQController.hh"

// Status:
// 0-idle
// 1-arming
// 2-armed
// 3-running
// 4-error

DAQController::DAQController(){
  fHelper = new DAXHelpers();
  fStatus = 0;
}
DAQController::~DAQController(){
  delete fHelper;
}

int DAQController::InitializeElectronics(Options *opts){

  fStatus = 1;
  for(auto d : opts->GetBoards("V1724")){
    
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
    for(auto regi : opts->GetRegisters(digi->bid())){
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
  fStatus = 2;
  return 0;
}

void DAQController::Start(){
  return;
}

void DAQController::Stop(){
  return;
}
void DAQController::End(){
  for(unsigned int x=0; x<fDigitizers.size(); x++){
    fDigitizers[x]->End();
    delete fDigitizers[x];
  }
  fDigitizers.clear();
  fStatus = 0;
}



