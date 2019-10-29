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
			  std::string host, std::string dac_collection,
			  bool debug){
  try{
    mongocxx::uri uri{connection_string};
    fMongoClient = mongocxx::client(uri);
    fMongoCollection = fMongoClient[db][collection];
    if(dac_collection != "")
      fDAC_collection = fMongoClient[db][dac_collection];
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

void MongoLog::UpdateDACDatabase(std::string run_identifier,
				 std::map<int,
				 std::vector<u_int16_t>>dac_values){
  using namespace bsoncxx::builder::stream;
  auto search_doc = document{} << "run" <<  run_identifier << finalize;
  auto update_doc = document{};
  update_doc<< "$set" << open_document << "run" << run_identifier;
  for(auto iter : dac_values){
    update_doc << std::to_string(iter.first) << open_array <<
      [&](array_context<> arr){
      for (u_int32_t i = 0; i < iter.second.size(); i++)
	arr << iter.second[i];
    } << close_array;
  }
  update_doc<<close_document;
  auto write_doc = update_doc<<finalize;
  mongocxx::options::update options;
  options.upsert(true);
  fDAC_collection.update_one(search_doc.view(), write_doc.view(), options);

}

int MongoLog::GetDACValues(int bid, int reference_run,
			   std::vector<u_int16_t> &dac_values){
  std::string runstring = std::to_string(reference_run);
  while(runstring.size()<6)
    runstring.insert(0, "0");

  bsoncxx::document::view res;
  if(reference_run >= 0){
    auto doc = fDAC_collection.find_one(bsoncxx::builder::stream::document{}<<
					"run" << runstring <<
					bsoncxx::builder::stream::finalize);
    if(!doc) return -1;
    res = (*doc).view();

    try{
      // Make sure key exists before loading
      if(res.find(std::to_string(bid)) != res.end()){
	dac_values.clear();
	bsoncxx::array::view channel_arr = res[std::to_string(bid)].get_array().value;
	for(bsoncxx::array::element ele : channel_arr)
	  dac_values.push_back(ele.get_int32());
	return 0;
      }
    }catch(...){
      dac_values = std::vector<u_int16_t>(16, 1000);
    }
  }
  else{ // either it's this driver or C++ but this is way harder than it should be
    auto sort_order = bsoncxx::builder::stream::document{} <<
      "_id" << -1 << bsoncxx::builder::stream::finalize;
    auto opts = mongocxx::options::find{};
    opts.sort(sort_order.view());    
    auto cursor = fDAC_collection.find({}, opts);
    auto doc = cursor.begin();
    if(doc==cursor.end()) // No docs
      return -1;
    res = *doc;
    
    try{
      // Make sure key exists before loading
      if(res.find(std::to_string(bid)) != res.end()){
	dac_values.clear();
	bsoncxx::array::view channel_arr = res[std::to_string(bid)].get_array().value;
	for(bsoncxx::array::element ele : channel_arr)
	  dac_values.push_back(ele.get_int32());
	return 0;
      }
    }catch(...){
      dac_values = std::vector<u_int16_t>(16, 4000);
    }
  }
    
  return -1;
}
