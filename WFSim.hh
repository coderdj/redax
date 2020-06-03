#ifndef _WFSIM_HH_
#define _WFSIM_HH_

#include "V1724.hh"
#include <thread>
#include <random>
#include <mutex>
#include <atomic>
#include <vector>

struct fax_options_t;

class WFSim : public V1724 {
public:
  WFSim(MongoLog* log, Options* options);
  virtual ~WFsim();

  virtual int Init(int, int, int, unsigned int=0);
  virtual int ReadMBLT(uint32_t*&);
  virtual int WriteRegister(unsigned int, unsigned int);
  virtual unsigned int ReadRegister(unsigned int);
  virtual int End();

private:
  void Run();
  std::tuple<double, double, double> GenerateEventLocation();
  std::tuple<int,int> GenerateEventSize(double, double, double);
  std::vector<std::pair<int, double>> MakeHitpattern(s_type, int, double, double, double);
  std::vector<std::vector<double>> MakeWaveform(std::vector<std::pair<int, double>>&);
  int ConvertToDigiFormat(std::vector<std::vector<double>>&, int);

  std::thread fGeneratorThread;
  std::string fBuffer;
  std::mutex fBufferMutex;
  std::atomic_int fBufferSize;
  std::random_device fRD;
  std::mt19937_64 fGen;
  std::uniform_real_distribution fFlatDist;
  long fClock;
  long fEventCounter;
  std::atomic_bool bRun;
  fax_options_t fFaxOptions;
  const int fNumPMTs;
  std::vector<double> fSPEtemplate;

  enum {
    S1,
    S2
  } s_type;
};

#endif // _WFSIM_HH_ defined
