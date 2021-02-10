#include "MongoLog.hh"
#include <iostream>
#include <chrono>
#include <bsoncxx/builder/stream/document.hpp>

namespace fs=std::experimental::filesystem;

MongoLog::MongoLog(int DeleteAfterDays, std::shared_ptr<mongocxx::pool>& pool, std::string dbname, std::string log_dir, std::string host, bool simple_structure) : 
  fPool(pool), fClient(pool->acquire()) {
  fLogLevel = 0;
  fHostname = host;
  fDeleteAfterDays = DeleteAfterDays;
  fFlushPeriod = 5; // seconds
  fTopOutputDir = log_dir;
  //fPool = pool;
  //fClient = pool->acquire();
  fDB = (*fClient)[dbname];
  fCollection = fDB["log"];
  fSimpleStructure = simple_structure;

  std::cout<<"Local file logging to " << log_dir << " with " (simple_structure ? "simple" : "manageable") << " structure\n";
  fFlush = true;
  fFlushThread = std::thread(&MongoLog::Flusher, this);
  fRunId = -1;

  RotateLogFile();

  fLogLevel = 1;
}

MongoLog::~MongoLog(){
  fFlush = false;
  fFlushThread.join();
  fOutfile.close();
}

void MongoLog::Flusher() {
  while (fFlush == true) {
    std::this_thread::sleep_for(std::chrono::seconds(fFlushPeriod));
    fMutex.lock();
    if (fOutfile.is_open()) fOutfile << std::flush;
    fMutex.unlock();
  }
}

std::string MongoLog::FormatTime(struct tm* date) {
  std::stringstream s;
  s <<std::put_time(date, "%F %T");
  return s.str();
}

int MongoLog::Today(struct tm* date) {
  return (date->tm_year+1900)*10000 + (date->tm_mon+1)*100 + (date->tm_mday);
}

std::string MongoLog::LogFileName(struct tm* date) {
  if (fSimpleStructure)
    return std::to_string(Today(date)) + "_" + fHostname + ".log";
  return fHostname + ".log";
}

fs::path OutputDir(const fs::path& toplevel, struct tm* date) {
  char temp[6];
  std::sprintf(temp, "%02d.%02d", tm->tm_mon+1, tm->tm_mday);
  return toplevel / std::string(temp);
}

int MongoLog::RotateLogFile() {
  if (fOutfile.is_open()) fOutfile.close();
  auto t = std::time(0);
  auto today = *std::gmtime(&t);
  std::string filename = LogFileName(&today);
  std::cout<<"Logging to " << fOutputDir/filename<<std::endl;
  fOutfile.open(fOutputDir / filename, std::ofstream::out | std::ofstream::app);
  if (!fOutfile.is_open()) {
    std::cout << "Could not rotate logfile!\n";
    return -1;
  }
  fOutfile << FormatTime(&today) << " [INIT]: logfile initialized\n";
  fToday = Today(&today);
  if (fDeleteAfterDays == 0) return 0;
  std::vector<int> days_per_month = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (today.tm_year%4 == 0) days_per_month[1] += 1; // the edge-case is SEP
  struct tm last_week = today;
  last_week.tm_mday -= fDeleteAfterDays;
  if (last_week.tm_mday <= 0) { // new month
    last_week.tm_mon--;
    if (last_week.tm_mon < 0) { // new year
      last_week.tm_year--;
      last_week.tm_mon = 11;
    }
    last_week.tm_mday += days_per_month[last_week.tm_mon]; // off by one error???
  }
  std::experimental::filesystem::path p = fOutputDir/LogFileName(&last_week);
  if (std::experimental::filesystem::exists(p)) {
    fOutfile << FormatTime(&today) << " [INIT]: Deleting " << p << '\n';
    std::experimental::filesystem::remove(p);
  } else {
    fOutfile << FormatTime(&today) << " [INIT]: No older logfile to delete :(\n";
  }
  return 0;
}

int MongoLog::Entry(int priority, std::string message, ...){

  // Thanks Martin
  // http://www.martinbroadhurst.com/string-formatting-in-c.html
  va_list args;
  va_start (args, message); // First pass just gets what the length will be
  size_t len = std::vsnprintf(NULL, 0, message.c_str(), args);
  va_end (args);
  std::vector<char> vec(len + 1); // Declare with proper length
  va_start (args, message);  // Fill the vector we just made
  std::vsnprintf(&vec[0], len + 1, message.c_str(), args);
  va_end (args);
  message = &vec[0];

  auto t = std::time(nullptr);
  auto tm = *std::gmtime(&t);
  std::stringstream msg;
  msg<<FormatTime(&tm)<<" ["<<fPriorities[priority+1] <<"]: "<<message<<std::endl;
  std::unique_lock<std::mutex> lg(fMutex);
  std::cout << msg.str();
  if (Today(&tm) != fToday) RotateLogFile();
  fOutfile<<msg.str();
  if(priority >= fLogLevel){
    try{
      auto d = bsoncxx::builder::stream::document{} <<
        "user" << fHostname <<
        "message" << message <<
        "priority" << priority <<
        "runid" << fRunId <<
        bsoncxx::builder::stream::finalize;
      fCollection.insert_one(std::move(d));
    }
    catch(const std::exception &e){
      std::cout<<"Failed to insert log message "<<message<<" ("<<
	priority<<")"<<std::endl;
      return -1;
    }
  }
  return 0;
}

