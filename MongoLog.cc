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
  auto tm = *std::localtime(&t);
  std::stringstream to_print;
  to_print<<std::put_time(&tm, "%Y-%m-%d %H-%M-%S")<<" ["<<fPriorities[priority+1]
	    <<"]: "<<message<<std::endl;
  std::cout << to_print.str();
  if(fLocalFileLogging){
    // ALL priorities get written locally (add some sort of size control later!)
    fOutfile<<to_print.str();
  }
  fMutex.unlock();
  
  return 0;
}
