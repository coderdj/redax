#include "MongoLog.hh"
#include <iostream>
#include <bsoncxx/builder/stream/document.hpp>

#ifndef REDAX_BUILD_COMMIT
#define REDAX_BUILD_COMMIT "UNKNOWN"
#endif

namespace fs=std::experimental::filesystem;

MongoLog::MongoLog(int DeleteAfterDays, std::shared_ptr<mongocxx::pool>& pool, std::string dbname, std::string log_dir, std::string host) : 
  fPool(pool), fClient(pool->acquire()) {
  fLogLevel = 0;
  fHostname = host;
  fDeleteAfterDays = DeleteAfterDays;
  fFlushPeriod = 5; // seconds
  fOutputDir = log_dir;
  fDB = (*fClient)[dbname];
  fCollection = fDB["log"];

  std::cout << "Local file logging to " << log_dir << std::endl;;
  fFlush = true;
  fFlushThread = std::thread(&MongoLog::Flusher, this);
  fRunId = -1;

  fLogLevel = 1;
}

MongoLog::~MongoLog(){
  fFlush = false;
  fCV.notify_one();
  fFlushThread.join();
  fOutfile.close();
}

std::tuple<struct tm, int> MongoLog::Now() {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto t = system_clock::to_time_t(now);
  int ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
  auto tm_ = *std::gmtime(&t);
  return {std::move(tm_), ms};
}

void MongoLog::Flusher() {
  // Logging from many threads occasionally segfaults when the messages are
  // coming in thick and fast, so this thread actually communicates with the db
  // while everything else just queues messages for handling
  int counter = 0;
  while (fFlush == true) {
    std::unique_lock<std::mutex> lk(fMutex);
    fCV.wait(lk, [&]{return fQueue.size() > 0 || fFlush == false;});
    if (fQueue.size()) {
      auto [today, ms, priority, message] = std::move(fQueue.front());
      fQueue.pop_front();
      lk.unlock();
      std::stringstream msg;
      msg<<FormatTime(&today, ms)<<" ["<<fPriorities[priority+1] <<"]: "<<message<<std::endl;
      if (Today(&today) != fToday) RotateLogFile();
      std::cout << msg.str();
      fOutfile << msg.str();
      if(priority >= fLogLevel){
        try{
          auto d = bsoncxx::builder::stream::document{} <<
            "user" << fHostname <<
            "message" << std::move(message) <<
            "priority" << priority <<
            "runid" << fRunId <<
            bsoncxx::builder::stream::finalize;
          fCollection.insert_one(std::move(d));
        }
        catch(const std::exception &e){
          std::cout<<"Failed to insert log message "<<message<<" ("<<
            priority<<")"<<std::endl;
          std::cout<<e.what()<<std::endl;
        }
      }
      if (++counter >= fFlushPeriod) {
        counter = 0;
        fOutfile << std::flush;
      }
    } else // queue not empty
      lk.unlock();
  } // while
}

std::string MongoLog::FormatTime(struct tm* date, int ms) {
  std::string out("YYYY-MM-DD HH:mm:SS.SSS");
  // this is kinda awkward but we can't use c++20's time-formatting gubbins so :(
  sprintf(out.data(), "%04i-%02i-%02i %02i:%02i:%02i.%03i", date->tm_year+1900,
      date->tm_mon+1, date->tm_mday, date->tm_hour, date->tm_min, date->tm_sec, ms);
  return out;
}

int MongoLog::Today(struct tm* date) {
  return (date->tm_year+1900)*10000 + (date->tm_mon+1)*100 + (date->tm_mday);
}

std::string MongoLog::LogFileName(struct tm* date) {
  return std::to_string(Today(date)) + "_" + fHostname + ".log";
}

fs::path MongoLog::LogFilePath(struct tm* date) {
  return OutputDirectory(date)/LogFileName(date);
}

fs::path MongoLog::OutputDirectory(struct tm*) {
  return fOutputDir;
}

int MongoLog::RotateLogFile() {
  if (fOutfile.is_open()) fOutfile.close();
  auto [today, ms] = Now();
  auto filename = LogFilePath(&today);
  std::cout<<"Logging to " << filename << std::endl;
  auto pp = filename.parent_path();
  if (!fs::exists(pp) && !fs::create_directories(pp)) {
    std::cout << "Could not create output directories for logging!" << std::endl;
    return -1;
  }
  fOutfile.open(filename, std::ofstream::out | std::ofstream::app);
  if (!fOutfile.is_open()) {
    std::cout << "Could not rotate logfile!\n";
    return -1;
  }
  fOutfile << FormatTime(&today, ms) << " [INIT]: logfile initialized: commit " << REDAX_BUILD_COMMIT << "\n";
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
  auto p = LogFileName(&last_week);
  if (std::experimental::filesystem::exists(p)) {
    fOutfile << FormatTime(&today, ms) << " [INIT]: Deleting " << p << '\n';
    std::experimental::filesystem::remove(p);
  } else {
    fOutfile << FormatTime(&today, ms) << " [INIT]: No older logfile to delete :(\n";
  }
  return 0;
}

int MongoLog::Entry(int priority, const std::string& message, ...){
  auto [today, ms] = Now();

  // Thanks Martin
  // http://www.martinbroadhurst.com/string-formatting-in-c.html
  va_list args;
  va_start (args, message);
  // First pass just gets what the length will be
  size_t len = std::vsnprintf(NULL, 0, message.c_str(), args);
  va_end (args);
  // Declare with proper length
  std::string msg(len + 1, 0);
  va_start (args, message);
  // Fill the new string we just made
  std::vsnprintf(msg.data(), len+1, message.c_str(), args);
  va_end (args);
  // strip the trailing \0
  msg.pop_back();
  {
    std::unique_lock<std::mutex> lg(fMutex);
    fQueue.emplace_back(std::make_tuple(std::move(today), ms, priority, std::move(msg)));
  }
  fCV.notify_one();
  return 0;
}


fs::path MongoLog_nT::OutputDirectory(struct tm* date) {
  char temp[6];
  std::sprintf(temp, "%02d.%02d", date->tm_mon+1, date->tm_mday);
  return fOutputDir / std::to_string(date->tm_year+1900) / std::string(temp);
}

std::string MongoLog_nT::LogFileName(struct tm*) {
  return fHostname + ".log";
}
