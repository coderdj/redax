#ifndef _V1495_HH_
#define _V1495_HH_

#include <CAENVMElib.h>
#include "MongoLog.hh"
#include "Options.hh"

//Register address definitions taken from XENON1T m_veto class in kodiaq
//https://github.com/coderdj/kodiaq and XENON1T DAQ m_veto config files

/*
#define V1495_ModuleReset               0x800A
#define V1495_MaskInA                   0x1020
#define V1495_MaskInB                   0x1024
#define V1495_MaskInD                   0x1028
#define V1495_MajorityThreshold         0x1014
#define V1495_CoincidenceWidth          0x1010
#define V1495_CTRL			0x1018
*/

class V1495{

public:
      V1495(std::shared_ptr<MongoLog>& log, std::shared_ptr<Options>& options, int bid, int handle, unsigned int address);
      virtual ~V1495();
      int WriteReg(unsigned int reg, unsigned int value);

private:
      int fBoardHandle, fBID;
      unsigned int fBaseAddress;
      std::shared_ptr<Options> fOptions;
      std::share_ptr<MongoLog> fLog;

};
#endif
