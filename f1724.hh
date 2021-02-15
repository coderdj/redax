#ifndef _F1724_HH_
#define _F1724_HH_

#include "V1724.hh"
#include "Options.hh"
#include <random>
#include <tuple>
#include <mutex>
#include <condition_variable>
#include <thread>

class f1724 : public V1724 {
public:
  f1724(std::shared_ptr<MongoLog>&, std::shared_ptr<Options>&, int, int, int, unsigned);
  virtual ~f1724();

  virtual int Read(std::unique_ptr<data_packet>&);
  virtual int WriteRegister(unsigned, unsigned);
  virtual unsigned ReadRegister(unsigned);
  virtual int End();

  virtual int SINStart();
  virtual int SoftwareStart();
  virtual int AcquisitionStop(bool);
  virtual int SWTrigger();
  virtual int Reset();
  virtual bool EnsureReady(int, int) {return sRun || sReady;}
  virtual bool EnsureStarted(int, int) {return sRun == true;}
  virtual bool EnsureStopped(int, int) {return sRun == false;}
  virtual int CheckErrors() {return fError ? 1 : 0;}
  virtual uint32_t GetAcquisitionStatus();

protected:
  struct hit_t {
    int pmt_i;
    double time;
  };
  struct pmt_pos_t {
    double x;
    double y;
    int array;
  };

  static void GlobalRun();
  static void GlobalInit(fax_options_t&, std::shared_ptr<MongoLog>&);
  static void GlobalDeinit();
  static std::tuple<double, double, double> GenerateEventLocation();
  static std::vector<int> GenerateEventSize(double, double, double);
  static std::vector<hit_t> MakeHitpattern(int, int, double, double, double);
  static void SendToWorkers(const std::vector<hit_t>&, long);
  static pmt_pos_t PMTiToXY(int);

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
  static std::vector<f1724*> sRegistry;
  static std::vector<pmt_pos_t> sPMTxy;
  static std::condition_variable sCV;
  static std::shared_ptr<MongoLog> sLog;

  virtual int Init(int, int, std::shared_ptr<Options>&);
  void Run();
  virtual int GetClockCounter(uint32_t);
  void MakeWaveform(std::vector<hit_t>&, long);
  void ConvertToDigiFormat(const std::vector<std::vector<double>>&, int, long);
  std::vector<std::vector<double>> GenerateNoise(int, int=0xFF);
  void ReceiveFromGenerator(std::vector<hit_t>, long);

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
  std::vector<hit_t> fProtoPulse;
  std::condition_variable fCV;
  std::mutex fMutex;
  std::thread fGeneratorThread;

  bool fSeenUnder5, fSeenOver15;
  bool fSimulateCrashes;
  double fFailProb, fCrashProb;
};

#endif // _F1724_HH_ defined
