#ifndef _WFSIM_HH_
#define _WFSIM_HH_

#include "V1724.hh"
#include "Options.hh"
#include <random>
#include <utility>
#include <tuple>

class WFSim : public V1724 {
public:
  WFSim(std::shared_ptr<ThreadPool>&, std::shared_ptr<Processor>&, std::shared_ptr<Options>&, std::shared_ptr<MongoLog>&);
  virtual ~WFSim();

  virtual int Init(int, int, int, unsigned int);
  virtual int Read(std::u32string* outptr=nullptr);
  virtual int WriteRegister(unsigned int, unsigned int);
  virtual unsigned int ReadRegister(unsigned int);
  virtual void End();

  virtual int SINStart();
  virtual int SoftwareStart();
  virtual int AcquisitionStop(bool);
  virtual int SWTrigger();
  virtual int Reset();
  virtual bool EnsureReady(int, int) {return sReady || sRun;}
  virtual bool EnsureStarted(int, int) {return sRun==true;}
  virtual bool EnsureStopped(int, int) {return sRun==false;}
  virtual int CheckErrors() {return 0;}
  virtual uint32_t GetAcquisitionStatus();

  virtual void Process(std::u32string_view);

protected:
  static void GlobalRun();
  static void GlobalInit(fax_options_t&, std::shared_ptr<MongoLog>&);
  static void GlobalDeinit();
  static std::tuple<double, double, double> GenerateEventLocation();
  static std::vector<int> GenerateEventSize(double, double, double);
  static std::vector<std::pair<int, double>> MakeHitpattern(int, int, double, double, double);
  static void SendToWorkers(const std::vector<std::pair<int, double>>&);

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
  static std::shared_ptr<MongoLog> sLog;

  virtual bool MonitorRegister(uint32_t, uint32_t, int, int, uint32_t) {return true;}
  virtual int GetClockCounter(uint32_t timestamp);
  void MakeWaveform(std::u32string_view);
  void ConvertToDigiFormat(const std::vector<std::vector<double>>&, int, long);
  std::vector<std::vector<double>> GenerateNoise(int, int=0xFF);

  std::u32string fBuffer;
  std::mutex fBufferMutex;
  std::atomic_int fBufferSize;
  std::random_device fRD;
  std::mt19937_64 fGen;
  std::uniform_real_distribution<> fFlatDist;
  std::vector<double> fSPEtemplate;
  std::vector<double> fBLoffset, fBLslope;
  std::vector<double> fNoiseRMS;
  std::vector<double> fBaseline;
  fax_options_t fFaxOptions;
  std::atomic_long fTimestamp;
  std::atomic_int fEventCounter;

  bool fSeenUnder5, fSeenOver15;
};

#endif // _WFSIM_HH_ defined
