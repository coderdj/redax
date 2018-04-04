#ifndef _DAQCONTROLLER_HH_
#define _DAQCONTROLLER_HH_

#include "V1724.hh"
#include "DAXHelpers.hh"
#include "Options.hh"

class DAQController{
  /*
    Main control interface for the DAQ. Control scripts and
    user-facing interfaces can call this directly.
  */
  
public:
  DAQController();
  ~DAQController();

  int InitializeElectronics(Options *opts);

  // Start run
  // Stop run
  // Get data (return new buffer and size)

  void End();
  
private:
  vector <V1724*> fDigitizers;
  bsoncxx::document::view fOptions;

  DAXHelpers *fHelper;
  
};

#endif
