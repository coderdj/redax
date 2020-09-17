#include <iostream>
#include <string>
#include <iomanip>
#include <csignal>
#include <thread>
#include <unistd.h>
#include <limits.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <getopt.h>
#include <memory>

#include "DAQController.hh"
#include "MongoLog.hh"
#include "Options.hh"

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

void UpdateStatus(std::string suri, std::string dbname, std::shared_ptr<DAQController>& controller) {
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
        "waiting" << controller->GetWaiting() <<
        "running" << controller->GetRunning() <<
	"buffer" << controller->GetBufferSize() <<
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
    << "--logdir <directory>: where to write the logs\n"
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

  std::string log_dir = "";
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
  auto logger = std::make_shared<MongoLog>(log_retention, log_dir);
  int ret = logger->Initialize(suri, dbname, "log", hostname, true);
  if(ret!=0){
    std::cout<<"Exiting"<<std::endl;
    exit(-1);
  }

  //Options
  std::shared_ptr<Options> fOptions;

  // The DAQController object is responsible for passing commands to the
  // boards and tracking the status
  auto controller = std::make_shared<DAQController>(logger, hostname);
  std::thread status_update(&UpdateStatus, suri, dbname, std::ref(controller));
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
	  } else
	    logger->Entry(MongoLog::Debug, "Cannot start DAQ since not in ARMED state");
	}
	else if(command == "stop"){
	  // "stop" is also a general reset command and can be called any time
	  if(controller->Stop()!=0)
	    logger->Entry(MongoLog::Error,
			  "DAQ failed to stop. Will continue clearing program memory.");
	  controller->End();
      fOptions.reset();
	}
	else if(command == "arm"){
	  // Can only arm if we're in the idle, arming, or armed state
	  if(controller->status() == 0 || controller->status() == 1 || controller->status() == 2){

	    // Join readout threads if they still are out there
	    controller->Stop();

	    // Clear up any previously failed things
	    if(controller->status() != 0)
	      controller->End();

	    // Get an override doc from the 'options_override' field if it exists
	    std::string override_json = "";
	    try{
	      bsoncxx::document::view oopts = (doc)["options_override"].get_document().view();
	      override_json = bsoncxx::to_json(oopts);
	    }catch(const std::exception &e){
	      logger->Entry(MongoLog::Debug, "No override options provided, continue without.");
	    }

	    fOptions = std::make_shared<Options>(logger, (doc)["mode"].get_utf8().value.to_string(),
				   hostname, suri, dbname, override_json);
	    if(controller->InitializeElectronics(fOptions) != 0){
	      logger->Entry(MongoLog::Error, "Failed to initialize electronics");
	      controller->End();
	    }else{
              logger->SetRunId(fOptions->GetString("run_identifier","none"));
	      logger->Entry(MongoLog::Debug, "Initialized electronics");
	    }
	  } // if status is ok
	  else
	    logger->Entry(MongoLog::Warning, "Cannot arm DAQ while not 'Idle'");
	} else if (command == "quit") b_run = false;
      } // for doc in cursor
    }
    catch(const std::exception &e){
      std::cout<<e.what()<<std::endl;
      std::cout<<"Can't connect to DB so will continue what I'm doing"<<std::endl;
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  status_update.join(); std::cout<<"Status update joined\n";
  controller.reset(); std::cout<<"Controller reset\n";
  fOptions.reset(); std::cout<<"Options reset\n";
  logger.reset(); std::cout<<"Logger reset\n";
  exit(0);

}

