#include <iostream>
#include <string>
#include <iomanip>
#include <csignal>
#include "DAQController.hh"
#include <thread>
#include <unistd.h>
#include "MongoLog.hh"
#include "Options.hh"
#include <limits.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <getopt.h>

#include <mongocxx/instance.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>

std::atomic_bool b_run = true;
std::string hostname = "";

void SignalHandler(int signum) {
    std::cout << "\nReceived signal "<<signum<<std::endl;
    b_run = false;
    return;
}

void UpdateStatus(std::string suri, std::string dbname, DAQController* controller) {
  mongocxx::uri uri(suri);
  mongocxx::client c(uri);
  mongocxx::collection status = c[dbname]["status"];
  using namespace std::chrono;
  while (b_run == true) {
    try{
      // Put in status update document
      auto insert_doc = bsoncxx::builder::stream::document{};
      insert_doc << "host" << hostname <<
        "time" << bsoncxx::types::b_date(system_clock::now())<<
	"rate" << controller->GetDataSize()/1e6 <<
	"status" << controller->status() <<
	"buffer_length" << controller->GetBufferLength() <<
        "buffer_size" << controller->GetBufferSize()/1e6 <<
        "strax_buffer" << controller->GetStraxBufferSize()/1e6 <<
	"run_mode" << controller->run_mode() <<
	"channels" << bsoncxx::builder::stream::open_document <<
	[&](bsoncxx::builder::stream::key_context<> doc){
	for( auto const& pair : controller->GetDataPerChan() )
	  doc << std::to_string(pair.first) << (pair.second>>10); // KB not MB
	} << bsoncxx::builder::stream::close_document;
	status.insert_one(insert_doc << bsoncxx::builder::stream::finalize);
    }catch(const std::exception &e){
      std::cout<<"Can't connect to DB to update."<<std::endl;
      std::cout<<e.what()<<std::endl;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  std::cout<<"Status update returning\n";
}

int PrintUsage() {
  std::cout<<"Welcome to REDAX readout\nAccepted command-line arguments:"
    << "--id <id number>: id number of this readout instance, required\n"
    << "--uri <mongo uri>: full MongoDB URI, required\n"
    << "--db <database name>: name of the database to use, default \"daq\"\n"
    << "--logdir <directory>: where to write the logs, default /live_data/redax_logs\n"
    << "--help: print this message\n"
    << "\n";
  return 1;
}

int main(int argc, char** argv){

  // Need to create a mongocxx instance and it must exist for
  // the entirety of the program. So here seems good.
  mongocxx::instance instance{};

  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);

  std::string current_run_id="none", log_dir = "/live_data/redax_logs";
  std::string dbname = "daq", suri = "", sid = "";
  int log_retention = 7; // days
  int c, opt_index;
  struct option longopts[] = {
    {"id", required_argument, 0, 0},
    {"uri", required_argument, 0, 1},
    {"db", required_argument, 0, 2},
    {"logdir", required_argument, 0, 3},
    {"help", no_argument, 0, 4}
  };
  while ((c = getopt_long(argc, argv, "", longopts, &opt_index)) != -1) {
    switch(c) {
      case 0:
        sid = optarg; break;
      case 1:
        suri = optarg; break;
      case 2:
        dbname = optarg; break;
      case 3:
        log_dir = optarg; break;
      case 4:
      default:
        std::cout<<"Received unknown arg\n";
        return PrintUsage();
    }
  }
  if (suri == "" || sid == "") return PrintUsage();

  // We will consider commands addressed to this PC's ID 
  char chostname[HOST_NAME_MAX];
  gethostname(chostname, HOST_NAME_MAX);
  hostname=chostname;
  hostname+= "_reader_" + sid;
  std::cout<<"Reader starting with ID: "<<hostname<<std::endl;

  // MongoDB Connectivity for control database. Bonus for later:
  // exception wrap the URI parsing and client connection steps
  mongocxx::uri uri(suri.c_str());
  mongocxx::client client(uri);
  mongocxx::database db = client[dbname];
  mongocxx::collection control = db["control"];
  mongocxx::collection status = db["status"];
  mongocxx::collection options_collection = db["options"];
  mongocxx::collection dac_collection = db["dac_calibration"];

  // Logging
  MongoLog *logger = new MongoLog(log_retention, log_dir);
  int ret = logger->Initialize(suri, dbname, "log", hostname, true);
  if(ret!=0){
    std::cout<<"Exiting"<<std::endl;
    exit(-1);
  }

  //Options
  Options *fOptions = NULL;
  
  // The DAQController object is responsible for passing commands to the
  // boards and tracking the status
  DAQController *controller = new DAQController(logger, hostname);
  std::vector<std::thread*> readoutThreads;
  std::thread status_update(&UpdateStatus, suri, dbname, controller);
  using namespace std::chrono;
  // Main program loop. Scan the database and look for commands addressed
  // to this hostname. 
  while(b_run == true){

    // Try to poll for commands
    bsoncxx::stdx::optional<bsoncxx::document::value> querydoc;

    try{

      // Sort oldest to newest
      auto order = bsoncxx::builder::stream::document{} <<
	"_id" << 1 <<bsoncxx::builder::stream::finalize;
      auto opts = mongocxx::options::find{};
      opts.sort(order.view());
      
      mongocxx::cursor cursor = control.find(
	 bsoncxx::builder::stream::document{} << "host" << hostname << "acknowledged." + hostname <<
	 bsoncxx::builder::stream::open_document << "$exists" << 0 <<
	 bsoncxx::builder::stream::close_document << 
	 bsoncxx::builder::stream::finalize, opts
	 );

      for(auto doc : cursor) {
	logger->Entry(MongoLog::Debug, "Found a doc with command %s",
	  doc["command"].get_utf8().value.to_string().c_str());
	// Very first thing: acknowledge we've seen the command. If the command
	// fails then we still acknowledge it because we tried
	control.update_one(
	   bsoncxx::builder::stream::document{} << "_id" << (doc)["_id"].get_oid() <<
	   bsoncxx::builder::stream::finalize,
	   bsoncxx::builder::stream::document{} << "$set" <<
	   bsoncxx::builder::stream::open_document << "acknowledged." + hostname <<
           (long)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() <<
	   bsoncxx::builder::stream::close_document <<
	   bsoncxx::builder::stream::finalize
	   );

	// Get the command out of the doc
	std::string command = "";
	std::string user = "";
	try{
	  command = (doc)["command"].get_utf8().value.to_string();
	  user = (doc)["user"].get_utf8().value.to_string();
	}
	catch (const std::exception &e){
	  //LOG
	  logger->Entry(MongoLog::Warning, "Received malformed command %s",
			bsoncxx::to_json(doc).c_str());
	}

	// Process commands
	if(command == "start"){

	  if(controller->status() == 2) {

	    if(controller->Start()!=0){
	      continue;
	    }

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

	    //logger->Entry(MongoLog::Message, "Received start command from user %s",
	//		  user.c_str());
	  }
	  else
	    logger->Entry(MongoLog::Debug, "Cannot start DAQ since not in ARMED state");
	}
	else if(command == "stop"){
	  // "stop" is also a general reset command and can be called any time
	  //logger->Entry(MongoLog::Message, "Received stop command from user %s",
	//		user.c_str());
	  if(controller->Stop()!=0)
	    logger->Entry(MongoLog::Error,
			  "DAQ failed to stop. Will continue clearing program memory.");

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

	    // Join readout threads if they still are out there
	    controller->Stop();
	    if(readoutThreads.size() !=0){
	      for(auto t : readoutThreads){
		logger->Entry(MongoLog::Local, "Joining orphaned readout thread");
		t->join();
		delete t;
	      }
	      readoutThreads.clear();
	    }

	    // Clear up any previously failed things
	    if(controller->status() != 0)
	      controller->End();

	    // Get an override doc from the 'options_override' field if it exists
	    std::string override_json = "";
	    try{
	      bsoncxx::document::view oopts = (doc)["options_override"].get_document().view();
	      override_json = bsoncxx::to_json(oopts);
	    }
	    catch(const std::exception &e){
	      logger->Entry(MongoLog::Debug, "No override options provided, continue without.");
	    }

	    bool initialized = false;

	    // Mongocxx types confusing so passing json strings around
	    if(fOptions != NULL) {
	      delete fOptions;
	      fOptions = NULL;
	    }
	    fOptions = new Options(logger, (doc)["mode"].get_utf8().value.to_string(),
				   hostname, suri, dbname, override_json);
	    std::vector<int> links;
	    if(controller->InitializeElectronics(fOptions, links) != 0){
	      logger->Entry(MongoLog::Error, "Failed to initialize electronics");
	      controller->End();
	    }
	  else{
	    initialized = true;
            logger->SetRunId(fOptions->GetString("run_identifier","none"));
	    logger->Entry(MongoLog::Debug, "Initialized electronics");
	  }
	    
	    if(readoutThreads.size()!=0){
	      logger->Entry(MongoLog::Message,
			    "Cannot start DAQ while readout thread from previous run active. Please perform a reset");
	    }
	    else if(!initialized){
	      logger->Entry(MongoLog::Warning, "Skipping readout configuration since init failed");
	    }
	    else{
	      controller->CloseProcessingThreads();
	      // open nprocessingthreads
	      if (controller->OpenProcessingThreads()) {
		logger->Entry(MongoLog::Warning, "Could not open processing threads!");
		controller->CloseProcessingThreads();
		throw std::runtime_error("Error while arming");
	      }
	      for(unsigned int i=0; i<links.size(); i++){
                readoutThreads.emplace_back(new std::thread(&DAQController::ReadData,
                      controller, links[i]));
	      }
	    }
	  } // if status is ok
	  else
	    logger->Entry(MongoLog::Warning, "Cannot arm DAQ while not 'Idle'");
	} else if (command == "quit") b_run = false;
      }
    }
    catch(const std::exception &e){
      std::cout<<e.what()<<std::endl;
      std::cout<<"Can't connect to DB so will continue what I'm doing"<<std::endl;
    }

    controller->CheckErrors();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  status_update.join();
  delete controller;
  if (fOptions != NULL) delete fOptions;
  delete logger;
  exit(0);

}



