#include <bsoncxx/array/view.hpp>
#include <bsoncxx/types.hpp>
#include <sstream>

#include "DAQController.hh"

DAQController::DAQController(){}
DAQController::~DAQController(){}

int DAQController::InitializeElectronics(bsoncxx::document::view opts){
  //std::cout<<bsoncxx::to_json(opts)<<std::endl;
  
  // Load digitizers from optiosn
  bsoncxx::array::view subarr = opts["boards"].get_array().value;
  for(bsoncxx::array::element ele : subarr){
    int link = u_int32_t(ele["link"].get_int32());
    int crate = u_int32_t(ele["crate"].get_int32());
    int bid = u_int32_t(ele["board"].get_int32());
    unsigned int vme = DAQController::StringToHex
      (std::string(ele["vme_address"].get_utf8().value.to_string()));

    V1724 *digi = new V1724();
    if(digi->Init(link, crate, bid, vme)==0){      
      fDigitizers.push_back(digi);     
    }
    else{
      std::cout<<"Failed to initialize digitizer "<<bid<<std::endl;
      exit(-1);
    }
  }
  
  // Load options to all digitizers
  bsoncxx::array::view regarr = opts["registers"].get_array().value;
  for(unsigned int x=0; x<fDigitizers.size(); x++){
    int success=0;
    for(bsoncxx::array::element ele : regarr){
      unsigned int reg = DAQController::StringToHex
	(std::string(ele["reg"].get_utf8().value.to_string()));
      unsigned int val = DAQController::StringToHex
	(std::string(ele["val"].get_utf8().value.to_string()));
      success+=fDigitizers[x]->WriteRegister(reg, val);
    }
    if(success!=0){
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

unsigned int DAQController::StringToHex(std::string str){
  stringstream ss(str);
  u_int32_t result;
  return ss >> std::hex >> result ? result : 0;
}
