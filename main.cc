#include <iostream>
#include <string>
#include "V1724.hh"
#include "DAQController.hh"
int main(){
  
  //std::cout<<"Hi"<<std::endl;

  std::string options = "{\"boards\": [{\"crate\": 0, \"link\": 0, \"board\": 100, \"vme_address\": \"0\"}], \"registers\": [{\"reg\": \"EF24\", \"val\": \"1\"}, {\"reg\": \"EF1C\", \"val\": \"1\"}, {\"reg\": \"EF00\", \"val\": \"10\"}, {\"reg\": \"8120\", \"val\": \"FF\"}, {\"reg\": \"8000\", \"val\": \"310\"}, {\"reg\": \"8080\", \"val\": \"310000\"}, {\"reg\": \"800C\", \"val\": \"A\"}, {\"reg\": \"8020\", \"val\": \"32\"}, {\"reg\": \"811C\", \"val\": \"110\"}, {\"reg\": \"8100\", \"val\": \"0\"}]}";
  DAQController baloo;
  auto bson_opts{bsoncxx::from_json(options).view()};
  baloo.InitializeElectronics(bson_opts);

  // READ DATA HERE
  
  baloo.End();


  exit(0);
  
  V1724 digitizer;
  // digitizer.Init(link, board, BID, baseAddress);
  digitizer.Init(0, 0, 100, 0);

  int r=0;
  r += digitizer.WriteRegister(0xEF24, 0x1);
  r += digitizer.WriteRegister(0xEF1C, 0x1);
  r += digitizer.WriteRegister(0xEF00, 0x10);
  r += digitizer.WriteRegister(0x8120, 0xFF);
  r += digitizer.WriteRegister(0x8000, 0x310);
  r += digitizer.WriteRegister(0x8080, 0x310000);
  r += digitizer.WriteRegister(0x800C, 0xA);
  r += digitizer.WriteRegister(0x8020, 0x32);
  r += digitizer.WriteRegister(0x811C, 0x110);
  r += digitizer.WriteRegister(0x8100, 0x0);

  if(r!=0){
    cout<<"DOH"<<endl;
    exit(-1);
  }

  

  digitizer.End();
  cout<<"Success"<<endl;
}


// Structure?
// DAQController - this is the monster
//               - hold all digitizer objects
//               - hold DAQ status object
//               - define with options document
// ControlDB     - poll this for commands
//               - send updates here too
