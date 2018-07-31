#ifndef _STRAXINSERTER_HH_
#define _STRAXINSERTER_HH_

#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <assert.h>
#include <blosc.h>

//#include "MongoInserter.hh"

//for debugging
#include <sys/types.h>
#include <unistd.h>
#include <sys/syscall.h>
#define gettid() syscall(SYS_gettid)

#include "StraxFileHandler.hh"

class DAQController;

struct data_packet{
  u_int32_t *buff;
  u_int32_t size;
  u_int32_t clock_counter;
  u_int32_t header_time;
  int bid;
};


class StraxInserter{
  /*
    Reformats raw data into strax format
  */
  
public:
  StraxInserter();
  ~StraxInserter();
  
  int  Initialize(Options *options, MongoLog *log, StraxFileHandler *handler,
		  DAQController *dataSource);
  void Close();
  
  int ReadAndInsertData();
  bool CheckError(){ return fErrorBit; };
private:
  int ParseDocuments(std::map<std::string, std::vector<char*>> &strax_docs,
		      data_packet dp);
  
  u_int64_t fChunkLength; // ns
  u_int32_t fChunkOverlap; // ns
  u_int16_t fFragmentLength; // This is in BYTES
  u_int16_t fStraxHeaderSize; // in BYTES too
  u_int32_t fChunkNameLength;
  Options *fOptions;
  MongoLog *fLog;
  DAQController *fDataSource;
  bool fActive;
  bool fErrorBit;

  StraxFileHandler *fStraxHandler;
};

#endif
