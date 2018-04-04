#include "DAQController.hh"

DAQController::DAQController(){
  fHelper = new DAXHelpers();
}
DAQController::~DAQController(){
  delete fHelper;
}

int DAQController::InitializeElectronics(Options *opts){

  for(auto d : opts->GetBoards("V1724")){      
    V1724 *digi = new V1724();
    if(digi->Init(d.link, d.crate, d.board, d.vme_address)==0){      
      fDigitizers.push_back(digi);     
    }
    else{
      std::cout<<"Failed to initialize digitizer "<<d.board<<std::endl;
      exit(-1);
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
      std::cout<<"Failed to write registers."<<std::endl;
      return -1;
    }
  }

  return 0;
}

void DAQController::End(){
  for(unsigned int x=0; x<fDigitizers.size(); x++){
    fDigitizers[x]->End();
    delete fDigitizers[x];
  }
  fDigitizers.clear();
}


