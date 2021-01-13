#ifndef _V1495_HH_
#define _V1495_HH_

#include <memory>
#include <map>

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

class MongoLog;
class Options;

class V1495{

public:
      V1495(std::shared_ptr<MongoLog>&, std::shared_ptr<Options>&, int, int, unsigned);
      virtual ~V1495();
      virtual int Init(std::map<std::string, int>&) {return 0;}
      int WriteReg(unsigned int, unsigned int);
      // Functions for a child class to implement
      virtual int BeforeSINStart() {return 0;}
      virtual int AfterSINStart() {return 0;}
      virtual int BeforeSINStop() {return 0;}
      virtual int AfterSINStop() {return 0;}

protected:
      int fBoardHandle, fBID;
      unsigned int fBaseAddress;
      std::shared_ptr<Options> fOptions;
      std::shared_ptr<MongoLog> fLog;
};
#endif
