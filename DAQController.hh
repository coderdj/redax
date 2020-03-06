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
class data_packet;

struct processingThread{
  std::thread *pthread;
  StraxInserter *inserter;
};

class DAQController{
  /*
    Main control interface for the DAQ. Control scripts and
    user-facing interfaces can call this directly.
  */
  
public:
  DAQController(MongoLog *log=NULL, std::string hostname="DEFAULT");
  ~DAQController();

  int InitializeElectronics(Options *options, std::vector<int> &keys);

  int status(){return fStatus;}
  int GetBufferLengths();
  std::string run_mode();
  
  int Start();
  int Stop();
  void ReadData(int link);
  void End();

  int GetData(std::list<data_packet*> &retVec, unsigned num = 0);
  int GetData(data_packet* &dp);
    
  // Static wrapper so we can call ReadData in a std::thread
  void ReadThreadWrapper(void* data, int link);
  void ProcessingThreadWrapper(void* data);

  int GetDataSize(){int ds = fDataRate; fDataRate=0; return ds;}
  std::map<int, int> GetDataPerChan();
  bool CheckErrors();
  void CheckError(int bid) {fCheckFails[bid] = true;}
  int OpenProcessingThreads();
  void CloseProcessingThreads();
  long GetStraxBufferSize();

  void GetDataFormat(std::map<int, std::map<std::string, int>>&);
  
private:

  void InitLink(std::vector<V1724*>&, std::map<int, std::map<std::string, std::vector<double>>>&, int&);
  int FitBaselines(std::vector<V1724*>&, std::map<int, std::vector<u_int16_t>>&, int,
      std::map<int, std::map<std::string, std::vector<double>>>&);
  
  std::vector <processingThread> fProcessingThreads;  
  std::map<int, std::vector <V1724*>> fDigitizers;
  std::list<data_packet*> fBuffer;
  std::mutex fBufferMutex;
  std::mutex fMapMutex;

  std::atomic_bool fReadLoop;
  std::map<int, std::atomic_bool> fRunning;
  int fStatus;
  int fNProcessingThreads;
  std::string fHostname;
  MongoLog *fLog;
  Options *fOptions;

  // For reporting to frontend
  std::atomic_int fBufferSize;
  std::atomic_int fBufferLength;
  std::atomic_int fDataRate;
  std::map<int, V1724*> fBoardMap;
  std::map<int, std::atomic_bool> fCheckFails;
};

#endif
