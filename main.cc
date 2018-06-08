#include <iostream>
#include <string>
#include "V1724.hh"
#include "DAQController.hh"

int main(int argc, char** argv){

  // Need to create a mongocxx instance and it must exist for
  // the entirety of the program. So here seems good.
  mongocxx::instance instance{};
  
  // We will consider commands addressed to this PC's hostname
  char hostname[HOST_NAME_MAX];
  gethostname(hostname, HOST_NAME_MAX);
  std::cout<<"Found hostname: "<<hostname<<std::endl;
  std::string current_run_id="none";
  
  // Accept just one command line argument, which is a URI
  if(argc==1){
    std::cout<<"Welcome to DAX. Run with a single argument: a valid mongodb URI"<<std::endl;
    std::cout<<"e.g. ./dax mongodb://user:pass@host:port/authDB"<<std::endl;
    std::cout<<"...exiting"<<std::endl;
    exit(0);
  }

  // MongoDB Connectivity for control database. Bonus for later:
  // exception wrap the URI parsing and client connection steps
  string suri = argv[1];  
  mongocxx::uri uri(suri.c_str());
  mongocxx::client client(uri);
  mongocxx::database db = client["dax"];
  mongocxx::collection control = db["control"];
  mongocxx::collection status = db["status"];
  mongocxx::collection options_collection = db["options"];

  // Logging
  MongoLog *logger = new MongoLog();
  int ret = logger->Initialize(suri, "dax", "log", true);
  if(ret!=0){
    std::cout<<"Exiting"<<std::endl;
    exit(-1);
  }
  
  // The DAQController object is responsible for passing commands to the
  // boards and tracking the status
  DAQController *controller = new DAQController(logger);  
  thread *readoutThread = NULL;
  
  // Main program loop. Scan the database and look for commands addressed
  // either to this hostname or with no hostname specified. Note: if you
  // send commands with no hostname specified the FIRST client to find the
  // command will run and delete it. So only do this if you have 1 client.
  while(1){

    mongocxx::cursor cursor = control.find(
        bsoncxx::builder::stream::document{} << "host" << hostname
	<< bsoncxx::builder::stream::finalize);
    for(auto doc : cursor) {

      // Get the command out of the doc
      string command = "";
      try{
	command = doc["command"].get_utf8().value.to_string();	
      }
      catch (const std::exception &e){
	//LOG
	std::stringstream err;
	err<<"Received malformed command: "<< bsoncxx::to_json(doc);
	logger->Entry(err.str(), MongoLog::Warning);
      }

      
      // Process commands
      if(command == "start"){

	if(controller->status() == 2) {
	  controller->Start();

	  // Nested tried cause of nice C++ typing
	  try{
	    current_run_id = doc["run_identifier"].get_utf8().value.to_string();	    
	  }
	  catch(const std::exception &e){
	    try{
	      current_run_id = std::to_string(doc["run_identifier"].get_int32());
	    }
	    catch(const std::exception &e){
	      current_run_id = "na";
	    }
	  }
	  
	  logger->Entry("Received start command from user "+
			doc["user"].get_utf8().value.to_string(), MongoLog::Message);
	}
	else
	  logger->Entry("Cannot start DAQ since not in ARMED state", MongoLog::Debug);
      }
      else if(command == "stop"){
	// "stop" is also a general reset command and can be called any time
	logger->Entry("Received stop command from user "+
		      doc["user"].get_utf8().value.to_string(), MongoLog::Message);
	controller->Stop();
	current_run_id = "none";
	if(readoutThread!=NULL){
	  readoutThread->join();
	  delete readoutThread;
	  readoutThread=NULL;
	}
	controller->End();
      }
      else if(command == "arm"){
	// Can only arm if we're in the idle, arming, or armed state
	if(controller->status() == 0 || controller->status() == 1 || controller->status() == 2){
	  controller->End();
	  string options = "";
	  try{
	    string option_name = doc["mode"].get_utf8().value.to_string();
	    logger->Entry("Loading options " + option_name, MongoLog::Debug);
	    
	    bsoncxx::stdx::optional<bsoncxx::document::value> trydoc =
	      options_collection.find_one(bsoncxx::builder::stream::document{}<<
					  "name" << option_name.c_str() <<
					  bsoncxx::builder::stream::finalize);
	    logger->Entry("Received arm command from user "+
			  doc["user"].get_utf8().value.to_string() +
			  " for mode " + option_name, MongoLog::Message);
	    if(trydoc){
	      options = bsoncxx::to_json(*trydoc);
	      if(controller->InitializeElectronics(options)!=0){
		logger->Entry("Failed to initialized electronics", MongoLog::Error);
	      }
	      else{
		logger->Entry("Initialized electronics", MongoLog::Debug);
	      }
	      
	      if(readoutThread!=NULL){
		logger->Entry("Cannot start DAQ while readout thread from previous run active. Please perform a reset", MongoLog::Message);
	      }
	      else
		readoutThread = new std::thread(DAQController::ReadThreadWrapper,
						(static_cast<void*>(controller))); 
	    }	  
	  }
	  catch( const std::exception &e){
	    logger->Entry("Want to arm boards but no valid mode provided", MongoLog::Warning);
	    options = "";	  
	  }
	}
	else
	  logger->Entry("Cannot arm DAQ while not 'Idle'", MongoLog::Warning);
      }


      // This command was processed (or failed) so remove it
      control.delete_one(bsoncxx::builder::stream::document{} << "_id" <<
			 doc["_id"].get_oid() << bsoncxx::builder::stream::finalize);
    }

    // Insert some information on this readout node back to the monitor DB
    controller->CheckErrors();
    status.insert_one(bsoncxx::builder::stream::document{} <<
		      "host" << hostname <<
		      "rate" << controller->GetDataSize()/1e6 <<
		      "status" << controller->status() <<
		      "buffer_length" << controller->buffer_length()/1e6 <<
		      "run_mode" << controller->run_mode() <<
		      "current_run_id" << current_run_id <<
		      bsoncxx::builder::stream::finalize);
    
    usleep(1000000);
  }
  delete logger;
  exit(0);
  

}



