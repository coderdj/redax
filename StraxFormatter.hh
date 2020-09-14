#ifndef _STRAXFORMATTER_HH_
#define _STRAXFORMATTER_HH_

#include <string>
#include <map>
#include <mutex>
#include <vector>
#include <exception>
#include "Processor.hh"
#include "ThreadPool.hh"

class InitException : public std::exception {};

class StraxFormatter : public Processor {
  /*
    Reformats raw data into strax format
  */

public:
  StraxFormatter(std::shared_ptr<ThreadPool>&, std::shared_ptr<Processor>&, std::shared_ptr<Options>&, std::shared_ptr<MongoLog>&);
  virtual ~StraxFormatter();

  std::map<int, int> GetDataPerChan();
  void Process(std::u32string_view);

private:
  int fFragmentBytes;
  int fStraxHeaderSize; // bytes
  int fSamplesPerFrag;
  std::map<int, std::atomic_int> fDataPerChan;
  std::mutex fMutex;
};

#endif // _STRAXFORMATTER_HH_ defined
