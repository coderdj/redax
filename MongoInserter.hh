#ifndef _MONGOINSERTER_HH_
#define _MONGOINSERTER_HH_

#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <assert.h>
#include <mongocxx/client.hpp>
#include <mongocxx/uri.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/database.hpp>
#include <mongocxx/collection.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include "Options.hh"
#include "MongoLog.hh"

class DAQController;

struct data_packet{
  u_int32_t *buff;
  u_int32_t size;
  u_int32_t clock_counter;
  u_int32_t header_time;
  int bid;
};


class MongoInserter{
  /*
    Reformats raw data into mongodb documents and inserts
  */
  
public:
  MongoInserter();
  ~MongoInserter();
  
  int  Initialize(Options *options, MongoLog *log, DAQController *dataSource);
  void Close();
  
  int ReadAndInsertData();
  bool CheckError(){ return fErrorBit; };
private:  
  static void ParseDocuments(std::vector<bsoncxx::document::value> &doc_array,
			     data_packet dp);
  
  //std::string FormatString(const std::string& format, ...);
  std::string FormatString(const std::string format,
			   const std::string pw);
  u_int32_t fBulkInsertSize;
  Options *fOptions;
  MongoLog *fLog;
  DAQController *fDataSource;
  bool fActive;
  bool fErrorBit;
};

#endif
