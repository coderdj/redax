#include "CControl_Handler.hh"

int main(int argc, char** argv){
  
  mongocxx::instance instance{};
  // Parse arguments
  if(argc < 3){
    std::cout<<"USAGE: ./ccontrol {ID} {MONGO_URI}"<<std::endl;
    std::cout<<"Where MONGO_URI is the URI of the command DB"<<std::endl;
    return -1;
  }

  // Control and status DB connectivity
  // We're going to poll control for commands
  // and update status with a heartbeat
  std::string mongo_uri = argv[2];
  mongocxx::uri uri(mongo_uri.c_str());
  mongocxx::client client(uri);
  mongocxx::database db = client["dax"];
  mongocxx::collection control = db["control"];
  mongocxx::collection status = db["status"];

  // Build a unique name for this process
  // we trust that the user has given {ID} uniquely
  char chostname[HOST_NAME_MAX];
  gethostname(chostname, HOST_NAME_MAX);
  std::string hostname = chostname;
  hostname += "_controller_";
  hostname += argv[1];
  std::cout<<"I dub thee "<<hostname<<std::endl;

  // Logging
  MongoLog *logger = new MongoLog();
  int ret = logger->Initialize(mongo_uri, "dax", "log", hostname, true);
  if(ret!=0){
    std::cout<<"Log couldn't be initialized. Exiting."<<std::endl;
    exit(-1);
  }

  // Holds session data
  CControl_Handler *fHandler = new CControl_Handler();  

  while(1){

    // Get documents with either "Start" or "Stop" commands
    mongocxx::cursor cursor = control.find
      (
       bsoncxx::builder::stream::document{}<<
       "command" <<
       bsoncxx::builder::stream::open_document <<
       "$in" <<  bsoncxx::builder::stream::open_array <<
       "start" << "send_stop_signal" << bsoncxx::builder::stream::close_array <<
       bsoncxx::builder::stream::close_document  <<
       "host" << hostname <<
       "acknowledged" <<
       bsoncxx::builder::stream::open_document <<
       "$ne" << hostname <<
       bsoncxx::builder::stream::close_document <<
       bsoncxx::builder::stream::finalize
       );
    
    for (auto doc : cursor) {
      
      // Acknowledge
      control.update_one
        (
	 bsoncxx::builder::stream::document{} << "_id" << doc["_id"].get_oid() <<
         bsoncxx::builder::stream::finalize,
         bsoncxx::builder::stream::document{} << "$push" <<
         bsoncxx::builder::stream::open_document << "acknowledged" << hostname <<
         bsoncxx::builder::stream::close_document <<
         bsoncxx::builder::stream::finalize
         );

      // Strip data from doc
      int run = -1;
      std::string command = "";
      std::string detector = "";
      try{
	command = doc["command"].get_utf8().value.to_string();
	detector = doc["detector"].get_utf8().value.to_string();
	run = doc["number"].get_int32();
      }
      catch(const std::exception E){
	logger->Entry("Received a document from the dispatcher missing [command|detector|run]",
		     MongoLog::Warning);
	logger->Entry(bsoncxx::to_json(doc), MongoLog::Warning);
	continue;
      }

      // If command is start then we need to have an options file
      std::string options = "";
      if(command == "start"){
	try{
	  options = doc["mode"].get_utf8().value.to_string();
	}
	catch(const std::exception E){
	  logger->Entry("Received a start document with no run mode",
			MongoLog::Warning);
	}
      }

      // Now we can process the command
      fHandler->ProcessCommand(command, detector, run, options);

    }

    // Report back what we doing
    status.insert_one(fHandler->GetStatusDoc(hostname));
    
    // Heartbeat and status info to monitor DB
    usleep(1000000);
  }
  
  return 0;
}
