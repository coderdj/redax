#include "MongoLog.hh"
#include <experimental/filesystem>
#include <iostream>

MongoLog::MongoLog(bool LocalFileLogging, int DeleteAfterDays){
  fLogLevel = 0;
  fHostname = "_host_not_set";
  fLogFileNameFormat = "%Y%m%d.log";
  fDeleteAfterDays = DeleteAfterDays;

  if(LocalFileLogging){
    std::cout<<"Configured WITH local file logging."<<std::endl;
    RotateLogFile();
  }
  fLocalFileLogging = LocalFileLogging;
}
MongoLog::~MongoLog(){
  fOutfile.close();
}

std::string MongoLog::FormatTime(struct tm* date) {
  std::stringstream s;
  s <<std::put_time(date, "%F %T");
  return s.str();
}

int MongoLog::Today(struct tm* date) {
  return (date->tm_year+1900)*10000 + (date->tm_mon+1)*100 + (date->tm_mday);
}

int MongoLog::RotateLogFile() {
  if (fOutfile.is_open()) fOutfile.close();
  auto t = std::time(0);
  auto today = *std::gmtime(&t);
  std::stringstream fn;
  fn << std::put_time(&today, fLogFileNameFormat.c_str());
  fOutfile.open(fn.str(), std::ofstream::out | std::ofstream::app);
  if (!fOutfile.is_open()) {
    std::cout << "Could not rotate logfile!\n";
    return -1;
  }
  fOutfile << FormatTime(&today) << " [INIT]: logfile initialized\n";
  fToday = Today(&today);
  std::array<int, 12> days_per_month = {31,28,31,30,31,30,31,31,30,31,30,31};
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
  std::stringstream s;
  s << std::put_time(&last_week, fLogFileNameFormat.c_str());
  std::experimental::filesystem::path p = s.str();
  if (std::experimental::filesystem::exists(p)) {
    fOutfile << FormatTime(&today) << " [INIT]: Deleting " << p << '\n';
    std::experimental::filesystem::remove(p);
  }
  else {
    fOutfile << FormatTime(&today) << " [INIT]: No older logfile to delete :(\n";
  }
  return 0;
}

int  MongoLog::Initialize(std::string connection_string,
			  std::string db, std::string collection,
			  std::string host,
			  bool debug){
  try{
    mongocxx::uri uri{connection_string};
    fMongoClient = mongocxx::client(uri);
    fMongoCollection = fMongoClient[db][collection];
  }
  catch(const std::exception &e){
    std::cout<<"Couldn't initialize the log. So gonna fail then."<<std::endl;
    return -1;
  }

  fHostname = host;

  if(debug)
    fLogLevel = 1;
  else
    fLogLevel = 0;

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

  fMutex.lock();
  if(priority >= fLogLevel){
    try{
      fMongoCollection.insert_one(bsoncxx::builder::stream::document{} <<
				  "user" << fHostname <<
				  "message" << message <<
				  "priority" << priority <<
				  bsoncxx::builder::stream::finalize);
      //std::cout<<"("<<priority<<"): "<<message<<std::endl;
    }
    catch(const std::exception &e){
      std::cout<<"Failed to insert log message "<<message<<" ("<<
	priority<<")"<<std::endl;
      return -1;
    }
  }

  auto t = std::time(nullptr);
  auto tm = *std::gmtime(&t);
  std::stringstream msg;
  msg<<FormatTime(&tm)<<" ["<<fPriorities[priority+1]
	    <<"]: "<<message<<std::endl;
  std::cout << msg.str();
  if(fLocalFileLogging){
    fMutex.lock();
    if (Today(&tm) != fToday) RotateLogFile();
    fOutfile<<msg.str();
    fMutex.unlock();
  }

  return 0;
}

