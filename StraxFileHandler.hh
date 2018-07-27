#ifndef _STRAXFILEHANDLER_HH_
#define _STRAXFILEHANDLER_HH_

#include <map>
#include <mutex>

#include <experimental/filesystem>

#include "MongoLog.hh"
#include "Options.hh"

class StraxFileHandler{

public:
  
  StraxFileHandler(MongoLog *log);
  ~StraxFileHandler();

  int Initialize(std::string output_path, std::string run_name, u_int32_t full_fragment_size,
		 std::string hostname);
  int InsertFragments(std::map<std::string, std::vector<unsigned char*> > parsed_fragments);
  void End();
  
private:

  MongoLog *fLog;
  std::experimental::filesystem::path fOutputPath;
  std::string fRunName;
  u_int32_t fFullFragmentSize;
  std::string fHostname;
  std::map<std::string, std::mutex>fFileMutexes;
  std::map<std::string, std::ofstream>fFileHandles;
};

#endif
