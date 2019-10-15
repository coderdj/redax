#ifndef _DAQCONTROLLER_HH_
#define _DAQCONTROLLER_HH_

#include <thread>
#include "V1724.hh"
#include "DAXHelpers.hh"
#include "Options.hh"
#include "StraxInserter.hh"

struct processingThread{
  std::thread *pthread;
  //MongoInserter *inserter;
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

  int InitializeElectronics(Options *options, std::vector<int> &keys,
			    std::map<int, std::vector<u_int16_t>>&written_dacs);

  int status(){
    return fStatus;
  };
  int buffer_length(){
    return fBufferLength;
  };
  std::string run_mode();    
  
  int Start();
  int Stop();
  void ReadData(int link);
  void End();

  int GetData(std::vector <data_packet> *&retVec);
    
  // Static wrapper so we can call ReadData in a std::thread
  static void* ReadThreadWrapper(void* data, int link);
  static void* ProcessingThreadWrapper(void* data);

  u_int64_t GetDataSize(){ u_int64_t ds = fDatasize; fDatasize=0; return ds;};
  std::map<int, u_int64_t> GetDataPerDigi();
  bool CheckErrors();
  void OpenProcessingThreads();
  void CloseProcessingThreads();

  std::map<std::string, int> GetDataFormat();
  
private:
  void AppendData(vector<data_packet> &d);
  
  std::vector <processingThread> fProcessingThreads;  
  std::map<int, std::vector <V1724*>> fDigitizers;
  std::mutex fBufferMutex;
  MongoLog *fLog;
  
  bool fReadLoop;
  Options *fOptions;
  DAXHelpers *fHelper;
  std::vector<data_packet> *fRawDataBuffer;
  int fStatus;
  int fNProcessingThreads;
  string fHostname;
  
  // For reporting to frontend
  u_int64_t fBufferLength;
  u_int64_t fDatasize;
  std::map<int, u_int64_t> fDataPerDigi;
  
  
};

#endif
