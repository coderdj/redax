#ifndef _V1724_HH_
#define _V1724_HH_

#include <cstdint>
#include <vector>
#include <map>

class MongoLog;
class Options;
class data_packet;

class V1724{

 public:
  V1724(MongoLog *log, Options *options);
  virtual ~V1724();

  int Init(int link, int crate, int bid, unsigned int address=0);
  int ReadMBLT(u_int32_t* &buffer, std::vector<unsigned int>* v=nullptr);
  int WriteRegister(unsigned int reg, unsigned int value);
  unsigned int ReadRegister(unsigned int reg);
  int GetClockCounter(u_int32_t timestamp, u_int32_t this_event_num);
  int End();

  int bid(){
    return fBID;
  };

  int LoadDAC(std::vector<u_int16_t> &dac_values);
  void ClampDACValues(std::vector<u_int16_t>&, std::map<std::string, std::vector<double>>&);
  unsigned GetNumChannels() {return fNChannels;}
  int SetThresholds(std::vector<u_int16_t> vals);

  // Acquisition Control
  int SINStart();
  int SoftwareStart();
  int AcquisitionStop();
  int SWTrigger();
  int Reset();
  bool EnsureReady(int ntries, int sleep);
  bool EnsureStarted(int ntries, int sleep);
  bool EnsureStopped(int ntries, int sleep);
  int CheckErrors();
  u_int32_t GetAcquisitionStatus();
  u_int32_t GetHeaderTime(u_int32_t *buff, u_int32_t size, u_int32_t& num);

  std::map<std::string, int> DataFormatDefinition;

protected:
  // Some values for base classes to override 
  unsigned int fAqCtrlRegister;
  unsigned int fAqStatusRegister;
  unsigned int fSwTrigRegister;
  unsigned int fResetRegister;
  unsigned int fChStatusRegister;
  unsigned int fChDACRegister;
  unsigned int fChTrigRegister;
  unsigned int fNChannels;
  unsigned int fSNRegisterMSB;
  unsigned int fSNRegisterLSB;
  unsigned int fBoardFailStatRegister;
  unsigned int fReadoutStatusRegister;
  unsigned int fVMEAlignmentRegister;
  unsigned int fBoardErrRegister;

  int BLT_SIZE;
  std::map<int, long> fBLTCounter;

  bool MonitorRegister(u_int32_t reg, u_int32_t mask, int ntries,
		       int sleep, u_int32_t val=1);
  Options *fOptions;
  int fBoardHandle;
  int fLink, fCrate, fBID;  
  unsigned int fBaseAddress;

  // Stuff for clock reset tracking
  u_int32_t clock_counter;
  u_int32_t last_time;
  u_int32_t last_event_num;
  bool seen_under_5;
  bool seen_over_15;

  MongoLog *fLog;

  float fBLTSafety, fBufferSafety;
  std::vector<uint32_t> fRO, fEV, fCH;
};


#endif
