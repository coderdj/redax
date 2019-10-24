#include "MongoLog.hh"

MongoLog::MongoLog(bool LocalFileLogging){
  fLogLevel = 0;
  fHostname = "_host_not_set";

  if(LocalFileLogging){
    std::cout<<"Configured WITH local file logging. See DAQLog.log"<<std::endl;
    fOutfile.open("DAQLog.log", std::ofstream::out | std::ofstream::app);
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);
    fOutfile<<std::put_time(&tm, "%d-%m-%Y %H-%M-%S")<<
      " [INIT]: File initialized"<<std::endl;
  }
  fLocalFileLogging = LocalFileLogging;
}
MongoLog::~MongoLog(){
  fOutfile.close();
};

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
      std::cout<<"("<<priority<<"): "<<message<<std::endl;
    }
    catch(const std::exception &e){
      std::cout<<"Failed to insert log message "<<message<<" ("<<
	priority<<")"<<std::endl;
      return -1;
    }
  }

  auto t = std::time(nullptr);
  auto tm = *std::localtime(&t);
  std::stringstream to_print;
  to_print<<std::put_time(&tm, "%d-%m-%Y %H-%M-%S")<<" ["<<fPriorities[priority+1]
	    <<"]: "<<message<<std::endl;
  std::cout << to_print.str();
  if(fLocalFileLogging){
    // ALL priorities get written locally (add some sort of size control later!)
    fOutfile<<to_print.str();
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
