#ifndef _MONGOINSERTER_HH_
#define _MONGOINSERTER_HH_

#include <unistd.h>
#include "Options.hh"

class DAQController;

struct data_packet{
  u_int32_t *buff;
  u_int32_t size;
  int bid;
};


class MongoInserter{
  /*
    Reformats raw data into mongodb documents and inserts
  */
  
public:
  MongoInserter();
  ~MongoInserter();
  
  int  Initialize(Options *options, DAQController *dataSource);
  void Close();
  
  int ReadAndInsertData();
  
  
private:
  Options *fOptions;
  DAQController *fDataSource;
  bool fActive;
};

#endif
