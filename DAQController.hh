#ifndef _DAQCONTROLLER_HH_
#define _DAQCONTROLLER_HH_

#include <mutex>
#include <thread>
#include "V1724.hh"
#include "DAXHelpers.hh"
#include "Options.hh"
#include "MongoInserter.hh"

class V2718; //not implemented yet

struct processingThread{
  std::thread *pthread;
  MongoInserter *inserter;
};

class DAQController{
  /*
    Main control interface for the DAQ. Control scripts and
    user-facing interfaces can call this directly.
  */
  
public:
  DAQController();
  ~DAQController();

  int InitializeElectronics(string opts);

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

  void Start();
  void Stop();
  void ReadData();
  void End();

  int GetData(std::vector <data_packet> *&retVec);
    
  // Statis wrapper so we can call ReadData in a std::thread
  static void* ReadThreadWrapper(void* data);
  static void* ProcessingThreadWrapper(void* data);

  u_int64_t GetDataSize(){ u_int64_t ds = fDatasize; fDatasize=0; return ds;};
  
private:
  void AppendData(vector<data_packet> d);
  
  vector <processingThread> fProcessingThreads;
  void OpenProcessingThreads();
  void CloseProcessingThreads();
  
  std::vector <V1724*> fDigitizers;
  std::mutex fBufferMutex;

  bool fReadLoop;
  Options *fOptions;
  DAXHelpers *fHelper;
  std::vector<data_packet> *fRawDataBuffer;
  int fStatus;
  int fNProcessingThreads;
  u_int64_t fBufferLength;
  u_int64_t fDatasize;
  
  V2718 *fRunStartController;
};

#endif
