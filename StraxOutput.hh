#ifndef _STRAXOUTPUT_HH_
#define _STRAXOUTPUT_HH_

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

class StraxOutput{
  /*
    Writes chunks to disk
  */
  
public:
  StraxInserter();
  ~StraxInserter();
  
  int  Initialize(Options *options, MongoLog *log, 
		  DAQController *dataSource, std::string hostname);

  void AddFragmentToBuffer(std::string&&, const std::string&);
  
private:
  void WriteOutChunk();

  std::experimental::filesystem::path GetFilePath(std::string id, bool temp);
  std::experimental::filesystem::path GetDirectoryPath(std::string id, bool temp);
  std::string GetStringFormat(int id);
  void CreateMissing(u_int32_t back_from_id);
  int fMissingVerified;

  int fBufferNumChunks;
  int fWarnIfChunkOlderThan;
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

  std::thread::id fThreadId;
};

#endif
