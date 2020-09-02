#ifndef _STRAXFORMATTER_HH_
#define _STRAXFORMATTER_HH_

#include <cstdlib>
#include <cstdint>
#include <string>

//for debugging
//#include <sys/types.h>
#include <map>
#include <mutex>
#include <numeric>
#include <atomic>
#include <vector>
#include <thread>
#include <exception>
#include "TheadPool.hh"

class InitException : public std::exception {};

class Options;
class MongoLog;

class StraxFormatter : public Processor {
  /*
    Reformats raw data into strax format
  */

public:
  StraxFormatter(ThreadPool*, Processor*, Options*, MongoLog*, std::map<int, std::map<std::string, int>>&);
  virtual ~StraxFormatter();

  void Close(std::map<int,int>& ret);

  bool CheckError(){ bool ret = fErrorBit; fErrorBit = false; return ret;}
  long GetBufferSize() {return fFragmentSize.load();}
  void GetDataPerChan(std::map<int, int>& ret);
  void CheckError(int bid);

  void Process(std::string_view);

private:
  void ProcessDatapacket(std::string_view);
  void ProcessEvent(uint32_t*, unsigned, long, uint32_t, int);
  void ProcessChannel(uint32_t*, unsigned, int, int, uint32_t, uint32_t, long, int);
  void GenerateArtificialDeadtime(int64_t, int16_t, uint32_t, int);

  int DPtoEvents(const uint32_t*, int);

  int64_t fChunkLength; // ns
  int64_t fChunkOverlap; // ns
  int fFragmentBytes;
  int fStraxHeaderSize; // bytes
  int fBufferNumChunks;
  int fWarnIfChunkOlderThan;
  unsigned fChunkNameLength;
  int64_t fFullChunkLength;
  std::string fOutputPath, fHostname;
  int fFragmentBytes; // This is in BYTES
  int fStraxHeaderSize; // in BYTES too
  Options *fOptions;
  MongoLog *fLog;
  DAQController *fDataSource;
  std::atomic_bool fActive, fRunning, fForceQuit;
  bool fErrorBit;
  std::atomic_long fFragmentSize;
  std::map<int, std::map<std::string, int>> fFmt;
  std::map<int, int> fFailCounter;
  std::mutex fFC_mutex;
  std::map<int, std::atomic_int> fDataPerChan;
  std::mutex fDPC_mutex;
  std::map<int, long> fBufferCounter;
  std::atomic_int fBufferLength;
  long fBytesProcessed;
  long fFragmentsProcessed;
  long fEventsProcessed;

  double fProcTimeDP, fProcTimeEv, fProcTimeCh, fCompTime;
  std::thread::id fThreadId;
};

#endif
