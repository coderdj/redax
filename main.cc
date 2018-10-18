#include <iostream>
#include <string>
#include <iomanip>
#include "V1724.hh"
#include "DAQController.hh"

int main(int argc, char** argv){

  // Need to create a mongocxx instance and it must exist for
  // the entirety of the program. So here seems good.
  mongocxx::instance instance{};
   
  std::string current_run_id="none";
  
  // Accept 2 arguments
  if(argc<3){
    std::cout<<"Welcome to DAX. Run with a unique ID and a valid mongodb URI"<<std::endl;
    std::cout<<"e.g. ./dax ID mongodb://user:pass@host:port/authDB"<<std::endl;
    std::cout<<"...exiting"<<std::endl;
    exit(0);
  }

  // We will consider commands addressed to this PC's ID 
  char chostname[HOST_NAME_MAX];
  gethostname(chostname, HOST_NAME_MAX);
  std::string hostname=chostname;
  hostname+= "_reader_";
  string sid = argv[1];
  hostname += sid;
  std::cout<<"Reader starting with ID: "<<hostname<<std::endl;
  
  // MongoDB Connectivity for control database. Bonus for later:
  // exception wrap the URI parsing and client connection steps
  string suri = argv[2];  
  mongocxx::uri uri(suri.c_str());
  mongocxx::client client(uri);
  mongocxx::database db = client["dax"];
  mongocxx::collection control = db["control"];
  mongocxx::collection status = db["status"];
  mongocxx::collection options_collection = db["options"];
  
  // Logging
  MongoLog *logger = new MongoLog();
  int ret = logger->Initialize(suri, "dax", "log", hostname, true);
  if(ret!=0){
    std::cout<<"Exiting"<<std::endl;
    exit(-1);
  }
  
  // The DAQController object is responsible for passing commands to the
  // boards and tracking the status
  DAQController *controller = new DAQController(logger, hostname);  
  std::vector<std::thread*> readoutThreads;
  
  // Main program loop. Scan the database and look for commands addressed
  // to this hostname. 
  while(1){

    // Try to poll for commands
    bsoncxx::stdx::optional<bsoncxx::document::value> querydoc;
    try{
      //   mongocxx::cursor cursor = control.find 
      querydoc = control.find_one
	(
	 bsoncxx::builder::stream::document{} << "host" << hostname << "acknowledged" <<
	 bsoncxx::builder::stream::open_document << "$ne" << hostname <<       
	 bsoncxx::builder::stream::close_document << 
	 bsoncxx::builder::stream::finalize
	 );
    }catch(const std::exception &e){
      std::cout<<e.what()<<std::endl;
      std::cout<<"Can't connect to DB so will continue what I'm doing"<<std::endl;
    }
    
    
    //for(auto doc : cursor) {
    if(querydoc){
      auto doc = querydoc->view();
      std::cout<<"Found a doc with command "<<
	doc["command"].get_utf8().value.to_string()<<std::endl;
      // Very first thing: acknowledge we've seen the command. If the command
      // fails then we still acknowledge it because we tried
      control.update_one
	(
	 bsoncxx::builder::stream::document{} << "_id" << (doc)["_id"].get_oid() <<
	 bsoncxx::builder::stream::finalize,
	 bsoncxx::builder::stream::document{} << "$push" <<
	 bsoncxx::builder::stream::open_document << "acknowledged" << hostname <<
	 bsoncxx::builder::stream::close_document <<
	 bsoncxx::builder::stream::finalize
	 );
      std::cout<<"Updated doc"<<std::endl;
      
      // Get the command out of the doc
      string command = "";
      try{
	command = (doc)["command"].get_utf8().value.to_string();	
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
	    current_run_id = (doc)["run_identifier"].get_utf8().value.to_string();	    
	  }
	  catch(const std::exception &e){
	    try{
	      current_run_id = std::to_string((doc)["run_identifier"].get_int32());
	    }
	    catch(const std::exception &e){
	      current_run_id = "na";
	    }
	  }
	  
	  logger->Entry("Received start command from user "+
			(doc)["user"].get_utf8().value.to_string(), MongoLog::Message);
	}
	else
	  logger->Entry("Cannot start DAQ since not in ARMED state", MongoLog::Debug);
      }
      else if(command == "stop"){
	// "stop" is also a general reset command and can be called any time
	logger->Entry("Received stop command from user "+
		      (doc)["user"].get_utf8().value.to_string(), MongoLog::Message);
	controller->Stop();
	current_run_id = "none";
	if(readoutThreads.size()!=0){
	  for(auto t : readoutThreads){
	    t->join();
	    delete t;
	  }
	  readoutThreads.clear();	
	}
	controller->End();
      }
      else if(command == "arm"){
	// Can only arm if we're in the idle, arming, or armed state
	if(controller->status() == 0 || controller->status() == 1 || controller->status() == 2){
	  
	  // Clear up any previously failed things
	  if(controller->status() != 0)
	    controller->End();
	  
	  // Try to pull options from database and store in an 'optional' object
	  bsoncxx::stdx::optional<bsoncxx::document::value> trydoc;
	  try{
	    string option_name = (doc)["mode"].get_utf8().value.to_string();
	    logger->Entry("Loading options " + option_name, MongoLog::Debug);
	    trydoc = options_collection.find_one(bsoncxx::builder::stream::document{}<<
						 "name" << option_name.c_str() <<
						 bsoncxx::builder::stream::finalize);	    
	    
	    logger->Entry("Received arm command from user "+
			  (doc)["user"].get_utf8().value.to_string() +
			  " for mode " + option_name, MongoLog::Message);
	  }
	  catch(const std::exception &e){
	    logger->Entry("Want to arm boards but no valid mode provided", MongoLog::Warning);
	  }


	  
	  // Get an override doc from the 'options_override' field if it exists
	  std::string override_json = "";
	  try{
	    bsoncxx::document::view oopts = (doc)["options_override"].get_document().view();
	    override_json = bsoncxx::to_json(oopts);
	  }
	  catch(const std::exception &e){
	    logger->Entry("No override options provided, continue without.", MongoLog::Debug);
	  }
	  
	  std::cout<<"Overrode JSON"<<std::endl;
	  
	  bool initialized = false;
	  if(trydoc){

	    // Pull all sub-docs
	    std::vector<std::string> include_json;
	    try{ 
              bsoncxx::array::view include_array = (*trydoc).view()["includes"].get_array().value;
	      for(bsoncxx::array::element ele : include_array){
		auto sd = options_collection.find_one(bsoncxx::builder::stream::document{}<<
						      "name" << ele.get_utf8().value.to_string() <<
						      bsoncxx::builder::stream::finalize);
		if(sd)
		  include_json.push_back(bsoncxx::to_json(*sd));
		else
		  logger->Entry("Possible improper run config. Options include documents faulty",
				MongoLog::Warning);
		  		
	      }
	    }
	    catch(...){};

	    // Mongocxx types confusing so passing json strings around
	    std::string options = bsoncxx::to_json(*trydoc);
	    std::vector<int> links;
	    std::cout<<"About to initialize electronics"<<std::endl;
	    if(controller->InitializeElectronics(options, links, include_json, override_json) != 0){
	      logger->Entry("Failed to initialize electronics", MongoLog::Error);
	      controller->End();
	    }
	    else{
	      initialized = true;
	      logger->Entry("Initialized electronics", MongoLog::Debug);
	    }
	    
	    if(readoutThreads.size()!=0){
	      logger->Entry("Cannot start DAQ while readout thread from previous run active. Please perform a reset", MongoLog::Message);
	    }
	    else if(!initialized){
	      cout<<"Skipping readout configuration since init failed"<<std::endl;
	    }
	    else{
	      for(unsigned int i=0; i<links.size(); i++){
		std::cout<<"Starting readout thread for link "<<links[i]<<std::endl;
		std:: thread *readoutThread = new std::thread
		  (
		   DAQController::ReadThreadWrapper,
		   (static_cast<void*>(controller)), links[i]
		   );
		readoutThreads.push_back(readoutThread);
	      }
	    }
	  }	  
	}
	else
	  logger->Entry("Cannot arm DAQ while not 'Idle'", MongoLog::Warning);
      }


      // This is in order to ensure concurrency. ALL documents will be purged if len(hosts)
      // is equal to len(acknowledged), i.e. every node acknowledged the command.
      // Note: usually '$where' is frowned upon due to it performing a collection scan each
      // time, but in this case it's totally fine since this collection will contain at most
      // like a few docs.
      // Note to self. Do we need this or just cap collection?
      try{
	control.delete_many(bsoncxx::builder::stream::document{} << "$where" <<
			    "this.host.length == this.acknowledged.length" <<
			    bsoncxx::builder::stream::finalize);
      }
      catch(const std::exception &e){
	std::cout<<"Error in delete_many "<<e.what()<<std::endl;
      }
    }
    // Insert some information on this readout node back to the monitor DB
    controller->CheckErrors();

    try{
      status.insert_one(bsoncxx::builder::stream::document{} <<
			"host" << hostname <<
			"rate" << controller->GetDataSize()/1e6 <<
			"status" << controller->status() <<
			"buffer_length" << controller->buffer_length()/1e6 <<
			"run_mode" << controller->run_mode() <<
			"current_run_id" << current_run_id <<
			bsoncxx::builder::stream::finalize);
    }catch(const std::exception &e){
      std::cout<<"Can't connect to DB to update."<<std::endl;
    }
    usleep(1000000);
  }
  delete logger;
  exit(0);
  

}





