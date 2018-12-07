#ifndef _DAQCONTROLLER_HH_
#define _DAQCONTROLLER_HH_

#include <thread>
#include "V1724.hh"
#include "DAXHelpers.hh"
#include "Options.hh"
#include "MongoInserter.hh"
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

  // Get data (return new buffer and size)
  double data_rate(){
    return 0;
  };
  int status(){
    return fStatus;
  };
  int buffer_length(){
    return fBufferLength;
  };
  std::string run_mode();    
  
  void Start();
  void Stop();
  void ReadData(int link);
  void End();

  int GetData(std::vector <data_packet> *&retVec);
    
  // Statis wrapper so we can call ReadData in a std::thread
  static void* ReadThreadWrapper(void* data, int link);
  static void* ProcessingThreadWrapper(void* data);

  u_int64_t GetDataSize(){ u_int64_t ds = fDatasize; fDatasize=0; return ds;};
  bool CheckErrors();
  
  
private:
  void AppendData(vector<data_packet> &d);
  
  vector <processingThread> fProcessingThreads;
  void OpenProcessingThreads();
  void CloseProcessingThreads();
  
  std::map<int, std::vector <V1724*>> fDigitizers;
  std::mutex fBufferMutex;
  MongoLog *fLog;
  
  bool fReadLoop;
  Options *fOptions;
  DAXHelpers *fHelper;
  std::vector<data_packet> *fRawDataBuffer;
  int fStatus;
  int fNProcessingThreads;
  u_int64_t fBufferLength;
  u_int64_t fDatasize;
  string fHostname;
  
  
};

#endif
