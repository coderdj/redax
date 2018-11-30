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
  mongocxx::collection options_collection = db["options"];

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

  // Options
  Options *options = NULL;
  
  // Holds session data
  CControl_Handler *fHandler = new CControl_Handler(logger, hostname);  

  while(1){
    mongocxx::cursor cursor = control.find
      (
       bsoncxx::builder::stream::document{}<< "host" << hostname <<"acknowledged" <<
       bsoncxx::builder::stream::open_document << "$ne" << hostname <<
       bsoncxx::builder::stream::close_document << bsoncxx::builder::stream::finalize
       );
    
    for (auto doc : cursor) {
      // Acknowledge the commands
      control.update_one
        (bsoncxx::builder::stream::document{} << "_id" << doc["_id"].get_oid() <<
         bsoncxx::builder::stream::finalize,
         bsoncxx::builder::stream::document{} << "$push" <<
         bsoncxx::builder::stream::open_document << "acknowledged" << hostname <<
         bsoncxx::builder::stream::close_document <<
         bsoncxx::builder::stream::finalize
         );
      
      // Strip data from the supplied doc
      int run = -1;
      std::string command = "";
      try{
	command = doc["command"].get_utf8().value.to_string();
	if(command == "arm" )
	  run = doc["number"].get_int32();
      }
      catch(const std::exception E){
        logger->Entry("ccontrol: Received a document from the dispatcher missing [command|number]",
		      MongoLog::Warning);
	continue;
      }
      
     // If the command is arm gonna use the options file to load the V2718, DDC10, etc...settings
     std::string mode = "";
     if(command == "arm"){
       try{
	 mode = doc["mode"].get_utf8().value.to_string();
       }
       catch(const std::exception E){
	 logger->Entry("ccontrol: Received an arm document with no run mode",MongoLog::Warning);
       }
                                     
       // Get an override doc from the 'options_override' field if it exists
       std::string override_json = "";
       try{
	 bsoncxx::document::view oopts = doc["options_override"].get_document().view();
	 override_json = bsoncxx::to_json(oopts);
       } 
       catch(const std::exception E){
	 logger->Entry("No override options provided", MongoLog::Debug);
       }	  
              
       //Here are our options
       if(options != NULL)
	 delete options;
       options = new Options(logger, mode, options_collection, override_json);
	 
       // Initialise the V2178, V1495 and DDC10...etc.      
       if(fHandler->DeviceArm(run, options) != 0){
	 logger->Entry("Failed to initialize devices", MongoLog::Error);
       }

     } // end if "arm" command
     

    else if(command == "start"){
       if((fHandler->DeviceStart()) != 0){
	 logger->Entry("Failed to start devices", MongoLog::Debug);
       }
     } 
     else if(command == "stop"){
       if((fHandler->DeviceStop()) != 0){
	 logger->Entry("Failed to stop devices", MongoLog::Debug);
       }
     } 
    } //end for  
 
    // Report back on what we are doing
    status.insert_one(fHandler->GetStatusDoc(hostname));
   usleep(1000000);
  }
  return 0;
}
