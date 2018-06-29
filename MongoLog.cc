#include "MongoLog.hh"

MongoLog::MongoLog(){
  fLogLevel = 0;
  
  char hostname[HOST_NAME_MAX];
  gethostname(hostname, HOST_NAME_MAX);
  fHostname = std::string(hostname);
}
MongoLog::~MongoLog(){};

int  MongoLog::Initialize(std::string connection_string,
		std::string db, std::string collection, bool debug){
  try{
    mongocxx::uri uri{connection_string};
    fMongoClient = mongocxx::client(uri);
    fMongoCollection = fMongoClient[db][collection];
  }
  catch(const std::exception &e){
    std::cout<<"Couldn't initialize the log. So gonna fail then."<<std::endl;
    return -1;
  }

  if(debug)
    fLogLevel = 1;
  else
    fLogLevel = 0;

  return 0;
}

int MongoLog::Entry(std::string message, int priority){

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
  return 0;
}



