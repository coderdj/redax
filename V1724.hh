#ifndef _V1724_HH_
#define _V1724_HH_

#include <cstdint>
#include <vector>
#include <map>
#include <chrono>
#include <tuple>
#include <string_view>
#include "ThreadPool.hh"

class MongoLog;
class Options;

class V1724 : public Processor{

 public:
  V1724(MongoLog *log, Options *options);
  virtual ~V1724();

  virtual int Init(int link, int crate, int bid, unsigned int address=0);
  virtual int ReadData(ThreadPool*);
  virtual int WriteRegister(unsigned int reg, unsigned int value);
  virtual unsigned int ReadRegister(unsigned int reg);
  virtual int End();

  int bid() {return fBID;}

  virtual int LoadDAC(std::vector<uint16_t> &dac_values);
  void ClampDACValues(std::vector<uint16_t>&, std::map<std::string, std::vector<double>>&);
  unsigned GetNumChannels() {return fNChannels;}
  int SetThresholds(std::vector<uint16_t> vals);

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

  virtual void Process(std::string_view);

  virtual std::tuple<int, int uint32_t> UnpackEventHeader(std::string_view);
  virtual std::tuple<int, int64_t, uint16_t> UnpackChannelHeader(std::string_view);

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
  uint32_t fLastClock;
  std::chrono::high_resolution_clock::time_point fLastClockTime;
  std::chrono::nanoseconds fClockPeriod;

  MongoLog *fLog;

  float fBLTSafety, fBufferSafety;

};

inline std::tuple<int, int, uint32_t> V1724::UnpackEventHeader(std::string_view sv) {
  // returns {words this event, channel mask, header timestamp}
  uint32_t* buff = (uint32_t*)sv.data();
  return {buff[0]&0xFFFFFFF, buff[1]&0xFF, buff[3]&0x7FFFFFFF};
}

inline std::tuple<int64_t, uint16_t, std::string_view> V1724::UnpackChannelHeader(std::string_vew sv, long rollovers, uint32_t) {
  // returns {timestamp, baseline, string view of the waveform}
  uint32_t* buff = (uint32_t*)sv.data();
  return {(rollovers<<31)+long(buff[1]&0x7FFFFFFF), 0, sv.substr(sv+2*sizeof(uint32_t))};
}

#endif
