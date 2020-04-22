#ifndef _STRAXINSERTER_HH_
#define _STRAXINSERTER_HH_

#include <cstdlib>
#include <cstdint>
#include <string>

//for debugging
//#include <sys/types.h>
#include <map>
#include <mutex>
#include <experimental/filesystem>
#include <numeric>
#include <atomic>
#include <vector>
#include <chrono>
#include <thread>

class DAQController;
class Options;
class MongoLog;

struct data_packet{
  public:
    data_packet();
    ~data_packet();
    u_int32_t *buff;
    int32_t size;
    u_int32_t clock_counter;
    u_int32_t header_time;
    int bid;
    std::vector<u_int32_t> vBLT;
};


class StraxInserter{
  /*
    Reformats raw data into strax format
  */
  
public:
  StraxInserter();
  ~StraxInserter();
  
  int  Initialize(Options *options, MongoLog *log, 
		  DAQController *dataSource, std::string hostname);
  void Close(std::map<int,int>& ret);
  
  int ReadAndInsertData();
  bool CheckError(){ bool ret = fErrorBit; fErrorBit = false; return ret;}
  long GetBufferSize() {return fFragmentSize.load();}
  void GetDataPerChan(std::map<int, int>& ret);
  void CheckError(int bid);
  int GetBufferLength() {return fBufferLength.load();}
  
private:
  void ProcessDatapacket(data_packet *dp);
  uint32_t ProcessEvent(uint32_t*, unsigned, long, uint32_t, int);
  int ProcessChannel(uint32_t*, unsigned, int, int, uint32_t, uint32_t, long, int);
  void WriteOutFiles(bool end=false);
  void GenerateArtificialDeadtime(int64_t timestamp, int16_t bid);
  void AddFragmentToBuffer(std::string& fragment, int64_t timestamp);

  std::experimental::filesystem::path GetFilePath(std::string id, bool temp);
  std::experimental::filesystem::path GetDirectoryPath(std::string id, bool temp);
  std::string GetStringFormat(int id);
  void CreateMissing(u_int32_t back_from_id);
  int fMissingVerified;

  int64_t fChunkLength; // ns
  int64_t fChunkOverlap; // ns
  int fFragmentBytes; // This is in BYTES
  int fStraxHeaderSize; // in BYTES too
  int fBufferNumChunks;
  unsigned fChunkNameLength;
  int64_t fFullChunkLength;
  std::string fOutputPath, fHostname;
  Options *fOptions;
  MongoLog *fLog;
  DAQController *fDataSource;
  std::atomic_bool fActive, fRunning, fForceQuit;
  bool fErrorBit;
  std::string fCompressor;
  std::map<std::string, std::string*> fFragments;
  std::atomic_long fFragmentSize;
  std::map<int, std::map<std::string, int>> fFmt;
  std::map<int, int> fFailCounter;
  std::mutex fFC_mutex;
  std::map<int, std::atomic_int> fDataPerChan;
  std::mutex fDPC_mutex;
  std::map<int, long> fBufferCounter;
  std::atomic_int fBufferLength;
  std::map<std::string, std::chrono::system_clock::time_point> fTimeLastSeen;
  long fBytesProcessed;
  long fFragmentsProcessed;
  long fEventsProcessed;
  bool fSawBit30;

  std::chrono::microseconds fProcTimeDP, fProcTimeEv, fProcTimeCh, fCompTime;
  std::thread::id fThreadId;
};

#endif
