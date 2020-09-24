#ifndef _DAQCONTROLLER_HH_
#define _DAQCONTROLLER_HH_

#include <thread>
#include <atomic>
#include <string>
#include <map>
#include <vector>
#include <cstdint>
#include <mutex>
#include <list>

class StraxInserter;
class MongoLog;
class Options;
class V1724;

class DAQController{
  /*
    Main control interface for the DAQ. Control scripts and
    user-facing interfaces can call this directly.
  */

public:
  DAQController(std::shared_ptr<MongoLog>&, std::string hostname="DEFAULT");
  ~DAQController();

  int InitializeElectronics(std::shared_ptr<Options>&);

  int status(){return fStatus;}
  int GetBufferLength();
  std::string run_mode();

  int Start();
  int Stop();
  void End();

  int GetDataSize(){int ds = fDataRate; fDataRate=0; return ds;}
  std::map<int, int> GetDataPerChan();
  void CheckError(int bid) {fCheckFails[bid] = true;}
  long GetStraxBufferSize();
  int GetBufferSize() {return fBufferSize.load();}

  void GetDataFormat(std::map<int, std::map<std::string, int>>&);

private:
  void ReadData(int link);
  int OpenProcessingThreads();
  void CloseProcessingThreads();
  void InitLink(std::vector<std::shared_ptr<V1724>>&, std::map<int, std::map<std::string, std::vector<double>>>&, int&);
  int FitBaselines(std::vector<std::shared_ptr<V1724>>&, std::map<int, std::vector<uint16_t>>&, int,
      std::map<int, std::map<std::string, std::vector<double>>>&);

  std::vector<std::unique_ptr<StraxInserter>> fFormatters;
  std::vector<std::thread> fProcessingThreads;
  std::map<int, std::vector<std::shared_ptr<V1724>>> fDigitizers;
  std::mutex fMapMutex;

  std::atomic_bool fReadLoop;
  std::map<int, std::atomic_bool> fRunning;
  int fStatus;
  int fNProcessingThreads;
  std::string fHostname;
  std::shared_ptr<MongoLog> fLog;
  std::shared_ptr<Options> fOptions;
  std::shared_ptr<ThreadPool> fTP;

  // For reporting to frontend
  std::atomic_int fBufferSize;
  std::atomic_int fBufferLength;
  std::atomic_int fDataRate;
  std::map<int, std::atomic_bool> fCheckFails;
};

#endif
