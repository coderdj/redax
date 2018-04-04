#include <iostream>
#include <string>
#include "V1724.hh"
#include "DAQController.hh"
int main(){
  
  DAQController baloo;
  Options *opts = new Options("defaults/options.ini");
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
