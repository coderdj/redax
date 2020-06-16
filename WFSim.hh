#ifndef _WFSIM_HH_
#define _WFSIM_HH_

#include "V1724.hh"
#include "Options.hh"
#include <thread>
#include <random>
#include <mutex>
#include <atomic>
#include <vector>
#include <utility>
#include <tuple>

enum s_type {
  S1 = 0,
  S2
} s_type;

class WFSim : public V1724 {
public:
  WFSim(MongoLog* log, Options* options);
  virtual ~WFSim();

  virtual int Init(int, int, int, unsigned int);
  virtual int ReadMBLT(uint32_t*&);
  virtual int WriteRegister(unsigned int, unsigned int);
  virtual unsigned int ReadRegister(unsigned int);
  virtual int End();

  virtual int SINStart() {return 1;} // not implemented yet
  virtual int SoftwareStart();
  virtual int AcquisitionStop() {fRun = false; return 0;}
  virtual int SWTrigger() {return 0;}
  virtual int Reset();
  virtual bool EnsureReady(int, int) {return true;}
  virtual bool EnsureStarted(int, int) {return fRun;}
  virtual bool EnsureStopped(int, int) {return fRun;}
  virtual int CheckErrors() {return 0;}
  virtual uint32_t GetAcquisitionStatus();

protected:
  virtual bool MonitorRegister(uint32_t, uint32_t, int, int, uint32_t) {return true;}
  void Run();
  std::tuple<double, double, double> GenerateEventLocation();
  std::tuple<int,int> GenerateEventSize(double, double, double);
  std::vector<std::pair<int, double>> MakeHitpattern(s_type, int, double, double, double);
  std::vector<std::vector<double>> MakeWaveform(std::vector<std::pair<int, double>>&, int&);
  int ConvertToDigiFormat(std::vector<std::vector<double>>&, int);

  std::thread fGeneratorThread;
  std::string fBuffer;
  std::mutex fBufferMutex;
  std::atomic_int fBufferSize;
  std::random_device fRD;
  std::mt19937_64 fGen;
  std::uniform_real_distribution<> fFlatDist;
  long fClock;
  int fEventCounter;
  std::atomic_bool fRun;
  fax_options_t fFaxOptions;
  const int fNumPMTs;
  std::vector<double> fSPEtemplate;

};

#endif // _WFSIM_HH_ defined
