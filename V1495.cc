#include "V1495.hh"
#include "MongoLog.hh"
#include "Options.hh"
#include <CAENVMElib.h>
#include "DAXHelpers.hh"


V1495::V1495(std::shared_ptr<MongoLog>& log, std::shared_ptr<Options>& options, int bid, int handle, unsigned int address){
  fOptions = options;
  fLog = log;
  fBID = bid;
  fBaseAddress = address;
  fBoardHandle = handle;
}

V1495::~V1495(){}

int V1495::Arm(std::map<std::string, int>&) {
  for (auto reg : fOptions->GetRegisters(fBID, true)) {
    if (WriteReg(DAXHelpers::StringToHex(reg.reg), DAXHelpers::StringToHex(reg.val))) {
      return -1;
    }
  }
  return 0;
}

int V1495::WriteReg(unsigned int reg, unsigned int value){
  int ret = 0;
  if((ret = CAENVME_WriteCycle(fBoardHandle, fBaseAddress+reg, &value, cvA32_U_DATA, cvD32)) != cvSuccess){
    fLog->Entry(MongoLog::Warning, "V1495: %i failed to write register 0x%04x with value %04x (%i)",
        fBID, reg, value, ret);
    return -1;
  }
  fLog->Entry(MongoLog::Local, "V1495: %i written register 0x%04x with value %04x",
      fBID, reg, value);
  return 0;
}

