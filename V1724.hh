#ifndef _V1724_HH_
#define _V1724_HH_

#include <cstdint>
#include <vector>
#include <map>
#include <tuple>
#include <string_view>
#include <atomic>
#include <memory>
#include "ThreadPool.hh"
#include "Processor.hh"


class V1724 : public Processor{

 public:
  V1724(std::shared_ptr<ThreadPool>&, std::shared_ptr<Processor>&, std::shared_ptr<Options>&, std::shared_ptr<MongoLog>&);
  virtual ~V1724();

  virtual int Init(int link, int crate, int bid, unsigned int address=0);
  virtual int Read(std::u32string* outptr=nullptr);
  virtual int WriteRegister(unsigned int reg, unsigned int value);
  virtual unsigned int ReadRegister(unsigned int reg);
  virtual void End();

  int bid() {return fBID;}

  virtual int LoadDAC(std::vector<uint16_t>&);
  void ClampDACValues(std::vector<uint16_t>&, std::map<std::string, std::vector<double>>&);
  unsigned GetNumChannels() {return fNChannels;}
  int SetThresholds(std::vector<uint16_t>);
  bool CheckFail() {bool ret = fCheckFail; fCheckFail = false; return ret;}

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

  virtual void Process(std::u32string_view);

  virtual std::tuple<int, int, bool, uint32_t> UnpackEventHeader(std::u32string_view);
  virtual std::tuple<int64_t, int, uint16_t, std::u32string_view> UnpackChannelHeader(std::u32string_view, long, uint32_t, uint32_t, int, int);

protected:
  uint32_t GetHeaderTime(char32_t*, int);
  virtual int GetClockCounter(uint32_t timestamp);
  bool MonitorRegister(uint32_t reg, uint32_t mask, int ntries, int sleep, uint32_t val=1);
  virtual void DPtoEvents(std::u32string_view);
  virtual void EventToChannels(std::u32string_view);
  void GenerateArtificialDeadtime(int64_t);

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
  int fArtificialDeadtimeChannel;

  int fClockCycle, fSampleWidth;

  int BLT_SIZE;
  std::map<int, long> fBLTCounter;

  int fBoardHandle;
  int fLink, fCrate, fBID;
  unsigned int fBaseAddress;

  // Stuff for clock reset tracking
  int fRolloverCounter;
  uint32_t fLastClock;
  std::chrono::high_resolution_clock::time_point fLastClockTime;
  std::chrono::nanoseconds fClockPeriod;


  float fBLTSafety;
  const int fDPoverhead, fEVoverhead, fCHoverhead;
  std::atomic_int fMissed, fFailures;
  std::atomic_bool fCheckFail;

};

#endif
