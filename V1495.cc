#include "V1495.hh"
#include "MongoLog.hh"
#include "Options.hh"
#include <CAENVMElib.h>


V1495::V1495(std::shared_ptr<MongoLog>& log, std::shared_ptr<Options>& options, int bid, int handle, unsigned int address){
  fOptions = options;
  fLog = log;
  fBID = bid;
  fBaseAddress = address;
  fBoardHandle = handle;
}

V1495::~V1495(){}

int V1495::WriteReg(unsigned int reg, unsigned int value){
  u_int32_t write=0;
  write+=value;
  if(CAENVME_WriteCycle(fBoardHandle, fBaseAddress+reg,
        &write,cvA32_U_DATA,cvD32) != cvSuccess){
    fLog->Entry(MongoLog::Warning, "V1495: %i failed to write register 0x%04x with value %08x (handle %i)", 
        fBID, reg, value, fBoardHandle);
    return -1;
  }
  fLog->Entry(MongoLog::Local, "V1495: %i written register 0x%04x with value %08x (handle %i)",
      fBID, reg, value, fBoardHandle);
  return 0;
}

