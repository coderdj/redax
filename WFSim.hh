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
#include <array>

class WFSim : public V1724 {
public:
  WFSim(MongoLog* log, Options* options);
  virtual ~WFSim();

  virtual int Init(int, int, int, unsigned int);
  virtual int ReadMBLT(uint32_t*&);
  virtual int WriteRegister(unsigned int, unsigned int);
  virtual unsigned int ReadRegister(unsigned int);
  virtual int End();

  virtual int SINStart();
  virtual int SoftwareStart();
  virtual int AcquisitionStop();
  virtual int SWTrigger() {return NoiseInjection();}
  virtual int Reset();
  virtual bool EnsureReady(int, int) {return sReady || sRun;}
  virtual bool EnsureStarted(int, int) {return fRun==true;}
  virtual bool EnsureStopped(int, int) {return fRun==false;}
  virtual int CheckErrors() {return 0;}
  virtual uint32_t GetAcquisitionStatus();

protected:
  static void GlobalRun();
  static void GlobalInit();
  static void GlobalDeinit();
  static std::tuple<double, double, double> GenerateEventLocation();
  static std::array<int, 3> GenerateEventSize(double, double, double);
  static std::vector<std::pair<int, double>> MakeHitpattern(int, int, double, double, double);
  static void SendToWorkers(const std::vector<std::pair<int, double>>&);
  static void WaitForSignal();

  static std::thread sGeneratorThread;
  static std::mutex sMutex;
  static std::random_device sRD;
  static std::mt19937_64 sGen;
  static std::uniform_real_distribution<> sFlatDist;
  static long sClock;
  static int sEventCounter;
  static std::atomic_bool sRun;
  static std::atomic_bool sReady;
  static fax_options_t sFaxOptions;
  static int sNumPMTs;
  static std::vector<WFSim*> sRegistry;
  static std::vector<std::pair<double, double>> sPMTxy;
  static std::condition_variable sCV;

  virtual bool MonitorRegister(uint32_t, uint32_t, int, int, uint32_t) {return true;}
  void Run();
  void ReceiveFromGenerator(const std::vector<std::pair<int, double>>&, long);
  std::vector<std::vector<double>> MakeWaveform(const std::vector<std::pair<int, double>>&, int&);
  void ConvertToDigiFormat(const std::vector<std::vector<double>>&, int);

  int NoiseInjection();

  std::thread fGeneratorThread;
  std::string fBuffer;
  std::mutex fBufferMutex;
  std::atomic_int fBufferSize;
  std::random_device fRD;
  std::mt19937_64 fGen;
  std::uniform_real_distribution<> fFlatDist;
  std::vector<double> fSPEtemplate;
  std::mutex fMutex;
  std::vector<std::pair<int, double>> fWFprimitive;
  std::condition_variable fCV;
  long fTimestamp;
  int fEventCounter;
};

#endif // _WFSIM_HH_ defined
