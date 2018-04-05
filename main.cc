#include <iostream>
#include <string>
#include "V1724.hh"
#include "DAQController.hh"
int main(int argc, char** argv){

  // Accept just one command line argument, which is an ini file. If you don't
  // provide an ini file the program automatically goes into remote control
  // mode and accepts commands via the database interface.
  if(argc==1){
    std::cout<<"Welcome to DAX. Running in remote control mode."<<std::endl;
    std::cout<<"But actually remote control mode is not implemented so I'm gonna quit..."<<std::endl;
    exit(0);
  }
  string ini_file = argv[1];
  Options *opts = NULL;
  try{
    opts = new Options(ini_file.c_str());
  }
  catch (std::runtime_error &e){
    std::cout << "EXITING: "<<e.what()<<std::endl;
    exit(0);
  }

  
  // Initialize the DAQ
  DAQController baloo;
  int ret = baloo.InitializeElectronics(opts);
  std::cout<<ret<<std::endl;
  // READ DATA HERE  
  baloo.End();


  exit(0);
  

}



// Structure?
// DAQController - this is the monster
//               - hold all digitizer objects
//               - hold DAQ status object
//               - define with options document
// ControlDB     - poll this for commands
//               - send updates here too
