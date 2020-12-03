#include <iostream>
#include <csignal>
#include "DAQController.hh"
#include "CControl_Handler.hh"
#include <thread>
#include <unistd.h>
#include "MongoLog.hh"
#include "Options.hh"
#include <chrono>
#include <thread>
#include <atomic>
#include <getopt.h>

#include <mongocxx/collection.hpp>
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

void UpdateStatus(std::string suri, std::string dbname, std::unique_ptr<DAQController>& controller) {
  mongocxx::uri uri(suri);
  mongocxx::client c(uri);
  mongocxx::collection status = c[dbname]["status"];
  using namespace std::chrono;
  while (b_run == true) {
    try{
      controller->StatusUpdate(&status);
    }catch(const std::exception &e){
      std::cout<<"Can't connect to DB to update."<<std::endl;
      std::cout<<e.what()<<std::endl;
    }
    std::this_thread::sleep_for(seconds(1));
  }
  std::cout<<"Status update returning\n";
}

int PrintUsage() {
  std::cout<<"Welcome to REDAX readout\nAccepted command-line arguments:\n"
    << "--id <id number>: id number of this readout instance, required\n"
    << "--uri <mongo uri>: full MongoDB URI, required\n"
    << "--db <database name>: name of the database to use, default \"daq\"\n"
    << "--logdir <directory>: where to write the logs, default pwd\n"
    << "--reader: this instance is a reader\n"
    << "--cc: this instance is a crate controller\n"
    << "--arm-delay <delay>: ms to wait between the ARM command and the arming sequence, default 15000\n"
    << "--log-retention <value>: how many days to keep logfiles, default 7\n"
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

  std::string current_run_id="none", log_dir = "";
  std::string dbname = "daq", suri = "", sid = "";
  bool reader = false, cc = false;
  int log_retention = 7; // days
  int c(0), opt_index, delay(15000);
  struct option longopts[] = {
    {"id", required_argument, 0, c++},
    {"uri", required_argument, 0, c++},
    {"db", required_argument, 0, c++},
    {"logdir", required_argument, 0, c++},
    {"reader", no_argument, 0, c++},
    {"cc", no_argument, 0, c++},
    {"arm-delay", required_argument, 0, c++},
    {"log-retention", required_argument, 0, c++},
    {"help", no_argument, 0, c++}
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
        reader = true; break;
      case 5:
        cc = true; break;
      case 6:
        delay = std::stoi(optarg); break;
      case 7:
        log_retention = std::stoi(optarg); break;
      case 8:
      default:
        std::cout<<"Received unknown arg\n";
        return PrintUsage();
    }
  }
  if (suri == "" || sid == "") return PrintUsage();
  if (reader == cc) {
    std::cout<<"Specify --reader XOR --cc\n";
    return 1;
  }

  // We will consider commands addressed to this PC's ID 
  const int HOST_NAME_MAX = 64; // should be #defined in unistd.h but isn't???
  char chostname[HOST_NAME_MAX];
  gethostname(chostname, HOST_NAME_MAX);
  hostname=chostname;
  hostname+= (reader ? "_reader_" : "_controller_") + sid;
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
  auto fLog = std::make_shared<MongoLog>(log_retention, log_dir, suri, dbname, "log", hostname);

  //Options
  std::shared_ptr<Options> fOptions;
  
  // The DAQController object is responsible for passing commands to the
  // boards and tracking the status
  std::unique_ptr<DAQController> controller;
  if (cc)
    controller = std::make_unique<CControl_Handler>(fLog, hostname);
  else
    controller = std::make_unique<DAQController>(fLog, hostname);
  std::thread status_update(&UpdateStatus, suri, dbname, std::ref(controller));

  using namespace bsoncxx::builder::stream;
  // Sort oldest to newest
  auto opts = mongocxx::options::find_one_and_update{};
  opts.sort(document{} << "_id" << 1 << finalize);
  auto query = document{} << "host" << hostname << "acknowledged." + hostname <<
    open_document << "$exists" << 0 << close_document << finalize;
  auto update = document{} << "$currentDate" << open_document <<
    "acknowledged."+hostname << true << close_document << finalize;
  using namespace std::chrono;
  // Main program loop. Scan the database and look for commands addressed
  // to this hostname. 
  while(b_run == true){
    // Try to poll for commands
    try{
      auto qdoc = control.find_one_and_update(query.view(), update.view(), opts);
      if (qdoc) {
	// Get the command out of the doc
        auto doc = qdoc->view();
	std::string command = "";
	std::string user = "";
	try{
	  command = (doc)["command"].get_utf8().value.to_string();
	  user = (doc)["user"].get_utf8().value.to_string();
	}
	catch (const std::exception &e){
	  fLog->Entry(MongoLog::Warning, "Received malformed command %s",
			bsoncxx::to_json(doc).c_str());
	}
	fLog->Entry(MongoLog::Debug, "Found a doc with command %s", command.c_str());
        auto ack_time = system_clock::now();

	// Process commands
	if(command == "start"){
	  if(controller->status() == 2) {
	    if(controller->Start()!=0){
	      continue;
	    }
            auto now = system_clock::now();
            fLog->Entry(MongoLog::Local, "Ack to start took %i us",
                duration_cast<microseconds>(now-ack_time).count());
	  }
	  else
	    fLog->Entry(MongoLog::Debug, "Cannot start DAQ since not in ARMED state (%i)", controller->status());
	}else if(command == "stop"){
	  // "stop" is also a general reset command and can be called any time
	  if(controller->Stop()!=0)
	    fLog->Entry(MongoLog::Error,
			  "DAQ failed to stop. Will continue clearing program memory.");
          auto now = system_clock::now();
          fLog->Entry(MongoLog::Local, "Ack to stop took %i us",
              duration_cast<microseconds>(now-ack_time).count());
          fLog->SetRunId(-1);
	} else if(command == "arm"){
	  // Can only arm if we're idle
	  if(controller->status() == 0){
	    controller->Stop();

	    // Get an override doc from the 'options_override' field if it exists
	    std::string override_json = "";
	    try{
	      auto oopts = doc["options_override"].get_document().view();
	      override_json = bsoncxx::to_json(oopts);
	    }
	    catch(const std::exception &e){
	    }
	    // Mongocxx types confusing so passing json strings around
            std::string mode = doc["mode"].get_utf8().value.to_string();
            fLog->Entry(MongoLog::Local, "Getting options doc for mode %s", mode.c_str());
	    fOptions = std::make_shared<Options>(fLog, mode,
				   hostname, suri, dbname, override_json);
            int dt = duration_cast<milliseconds>(system_clock::now()-ack_time).count();
            fLog->SetRunId(fOptions->GetInt("number", -1));
            fLog->Entry(MongoLog::Local, "Took %i ms to load config", dt);
            if (dt < delay) std::this_thread::sleep_for(milliseconds(delay-dt));
	    if(controller->Arm(fOptions) != 0){
	      fLog->Entry(MongoLog::Error, "Failed to initialize electronics");
	      controller->Stop();
	    }else{
	      fLog->Entry(MongoLog::Debug, "Initialized electronics");
	    }
	  } // if status is ok
	  else
	    fLog->Entry(MongoLog::Warning, "Cannot arm DAQ while not 'Idle'");
	} else if (command == "quit") b_run = false;
      } // if doc
    }catch(const std::exception &e){
      std::cout<<e.what()<<std::endl;
      std::cout<<"Can't connect to DB so will continue what I'm doing"<<std::endl;
    }

    std::this_thread::sleep_for(milliseconds(100));
  }
  status_update.join();
  controller.reset();
  fOptions.reset();
  fLog.reset();
  exit(0);
}

