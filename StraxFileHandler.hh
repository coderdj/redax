#ifndef _STRAXFILEHANDLER_HH_
#define _STRAXFILEHANDLER_HH_

#include <map>
#include <mutex>
#include <blosc.h>
#include <experimental/filesystem>

#include "MongoLog.hh"
#include "Options.hh"

class StraxFileHandler{

public:
  
  StraxFileHandler(MongoLog *log);
  ~StraxFileHandler();

  int Initialize(std::string output_path, std::string run_name, u_int32_t full_fragment_size,
		 std::string hostname);
  int InsertFragments(std::map<std::string, std::string*> &parsed_fragments);
  void End(bool silent=false);
  
private:

  std::experimental::filesystem::path GetFilePath(std::string id, bool temp);
  std::experimental::filesystem::path GetDirectoryPath(std::string id, bool temp);
  std::string GetStringFormat(int id);

  // One day these should return some statement on their success or failure
  void CleanUp(u_int32_t back_from_id, bool force_all=false);
  void CreateMissing(u_int32_t back_from_id);
  void FinishFile(std::string chunk_index);
  
  MongoLog *fLog;
  std::experimental::filesystem::path fOutputPath;
  std::string fRunName;
  u_int32_t fFullFragmentSize, fCleanToId;
  std::string fHostname;
  std::map<std::string, std::mutex>fFileMutexes;
  std::map<std::string, std::ofstream>fFileHandles;
  u_int32_t fChunkCloseDelay, fChunkNameLength;
  std::mutex fChunkCloseMutex, fNewElementMutex;
};

#endif
