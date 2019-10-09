#ifndef _V2718_HH_
#define _V2718_HH_


#include <CAENVMElib.h>
#include "MongoLog.hh"
#include "Options.hh"

//#include <unistd.h>
//#include <cstring>
//#include <stdint.h>
//#include <vector>
//#include <iostream>

using namespace std;

class V2718{
 
public:
  V2718(MongoLog *log);
  ~V2718();
  
  int CrateInit(CrateOptions c_opts, int link, int crate, uint32_t vme_address);
  int SendStartSignal();
  int SendStopSignal(bool end=true); 
  
  CrateOptions GetCrateOptions(){ return fCopts;};
  
private:
  
  CrateOptions fCopts;
  
  int fBoardHandle;
  int fCrate, fLink;
  uint32_t fVMEAddress;
  
  MongoLog *fLog;

};

#endif



