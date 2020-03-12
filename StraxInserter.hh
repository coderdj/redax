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
    data_packet() : buff(nullptr), size(0), bid(0) {}
    ~data_packet() {
      if (buff != nullptr) delete[] buff;
      buff = nullptr;
      size = bid = 0;
    }
    u_int32_t *buff;
    int32_t size;
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
  
  int  Initialize(Options *options, MongoLog *log, int bid,
		  DAQController *dataSource, std::string hostname);
  void Close(std::map<int,int>& ret);
  
  int ReadAndInsertData();
  bool CheckError(){ bool ret = fErrorBit; fErrorBit = false; return ret; }
  long GetBufferSize();
  void GetDataPerChan(std::map<int, int>& ret);
  int GetBufferLength() {return fBufferLength.load();}
  int ID() {return fBID;}
  
private:
  void ParseDocuments(data_packet *dp);
  void WriteOutFiles(int smallest_index_seen);
  void End();
  int64_t HandleClockRollovers(int, u_int32_t);
  int AddFragmentToBuffer(std::string&, int64_t);
  void GenerateArtificialDeadtime(int64_t);

  std::experimental::filesystem::path GetFilePath(std::string id, bool temp);
  std::experimental::filesystem::path GetDirectoryPath(std::string id, bool temp);
  std::string GetStringFormat(int id);
  void CreateMissing(u_int32_t back_from_id);
  int fMissingVerified;

  u_int64_t fChunkLength; // ns
  u_int32_t fChunkOverlap; // ns
  u_int16_t fFragmentBytes; // This is in BYTES
  u_int16_t fStraxHeaderSize; // in BYTES too
  u_int32_t fChunkNameLength;
  int64_t fFullChunkLength;
  std::string fOutputPath, fHostname;
  Options *fOptions;
  MongoLog *fLog;
  DAQController *fDataSource;
  std::atomic_bool fActive, fRunning;
  bool fErrorBit;
  int fBID;
  std::string fCompressor;
  std::map<std::string, std::string*> fFragments;
  std::map<std::string, std::atomic_long> fFragmentSize;
  std::map<std::string, int> fFmt;
  int fFailCounter;
  std::map<int, std::atomic_int> fDataPerChan;
  std::map<int, long> fBufferCounter;
  std::atomic_int fBufferLength;
  long fBytesProcessed;
  std::vector<u_int32_t> fLastTimeSeen;
  std::vector<long> fClockRollovers;

  std::chrono::microseconds fProcTime;
  std::chrono::microseconds fCompTime;
};

#endif
