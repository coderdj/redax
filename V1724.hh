#ifndef _V1724_HH_
#define _V1724_HH_

#include <cstdint>
#include <vector>
#include <map>
#include <chrono>

class MongoLog;
class Options;
class data_packet;
class ThreadPool;

class V1724{

 public:
  V1724(MongoLog *log, Options *options);
  virtual ~V1724();

  virtual int Init(int link, int crate, int bid, unsigned int address=0);
  virtual int ReadData(ThreadPool*);
  virtual int WriteRegister(unsigned int reg, unsigned int value);
  virtual unsigned int ReadRegister(unsigned int reg);
  virtual int End();

  int bid() {return fBID;}

  virtual int LoadDAC(std::vector<u_int16_t> &dac_values);
  void ClampDACValues(std::vector<u_int16_t>&, std::map<std::string, std::vector<double>>&);
  unsigned GetNumChannels() {return fNChannels;}
  int SetThresholds(std::vector<u_int16_t> vals);

  // Acquisition Control

  virtual int SINStart();
  virtual int SoftwareStart();
  virtual int AcquisitionStop(bool=false);
  virtual int SWTrigger();
  virtual int Reset();
  virtual bool EnsureReady(int ntries, int sleep);
  virtual bool EnsureStarted(int ntries, int sleep);
  virtual bool EnsureStopped(int ntries, int sleep);
  virtual int CheckErrors();
  virtual uint32_t GetAcquisitionStatus();

  std::map<std::string, int> DataFormatDefinition;

protected:
  uint32_t GetHeaderTime(uint32_t *buff, int size);
  virtual int GetClockCounter(uint32_t timestamp);
  bool MonitorRegister(uint32_t reg, uint32_t mask, int ntries, int sleep, uint32_t val=1);
  void DPtoEvents(std::string&);
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

  Options *fOptions;
  int fBoardHandle;
  int fLink, fCrate, fBID;
  unsigned int fBaseAddress;

  // Stuff for clock reset tracking
  int fRolloverCounter;
  u_int32_t fLastClock;
  std::chrono::high_resolution_clock::time_point fLastClockTime;
  std::chrono::nanoseconds fClockPeriod;

  MongoLog *fLog;

  float fBLTSafety, fBufferSafety;

};


#endif
