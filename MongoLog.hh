#ifndef _MONGOLOG_HH_
#define _MONGOLOG_HH_

#include <unistd.h>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <string>
#include <map>
#include <vector>
#include <atomic>
#include <thread>
#include <experimental/filesystem>

#include <mongocxx/collection.hpp>

/* 
   A brief treatise on log priorities. 

   LOCAL: the debuggiest debug messages possible. Only written to 
   local file and not uploaded to the database. Nobody will ever
   want to read these except you in the next 5 minutes and maybe 
   not even you. The default running mode does not record these at all.

   DEBUG: Actual debug stuff that you may want to leave on during
   commissioning. Will be uploaded to the DB. A very good idea would
   be to configure a TTL index (or some similar functionality) on 
   messages containing this priority so they get deleted after, say, 
   a month. Then just leave on forever.

   WARNING: little oops. Bad thing happened but it is concievable 
   the DAQ will continue without intervention if it just auto restarts. 
   An example would be if you try to start with a run mode this process
   cannot find.

   ERROR: big oops. The DAQ will not recover from this without manual
   intervention. An example would be like a disk full error or something
   along those lines.

   FATAL: reserved for something to write out right before crashing and 
   dying. Like a last gasp before giving up the ghost. These are the words
   on your process' tombstone. An operator shouldn't expect to find a running
   *process.
*/

class MongoLog{
  /*
    Logging class that writes to MongoDB
  */

public:
  MongoLog(int DeleteAfterDays, mongocxx::collection*, std::string);
  ~MongoLog();
  
  int  Initialize(std::string connection_string,
		  std::string db, std::string collection,
		  std::string host, bool debug=false);

  const static int Debug   = 0;  // Verbose output
  const static int Message = 1;  // Normal output
  const static int Warning = 2;  // Bad but minor operational impact
  const static int Error   = 3;  // Major operational impact
  const static int Fatal   = 4;  // Program gonna die
  const static int Local   = -1; // Write to local (file) log only

  int Entry(int priority,std::string message, ...);
  void SetRunId(const int runid) {fRunId = runid;}

private:
  void Flusher();
  std::string FormatTime(struct tm* date);
  int Today(struct tm* date);
  int RotateLogFile();
  std::string LogFileName(struct tm* date);
  std::vector<std::string> fPriorities{"LOCAL", "DEBUG", "MESSAGE",
      "WARNING", "ERROR", "FATAL"};
  std::ofstream fOutfile;
  mongocxx::collection* fMongoCollection;
  std::string fHostname;
  int fLogLevel;
  int fDeleteAfterDays;
  int fToday;
  std::mutex fMutex;
  std::experimental::filesystem::path fOutputDir;
  std::thread fFlushThread;
  std::atomic_bool fFlush;
  int fFlushPeriod;
  int fRunId;
};

#endif
