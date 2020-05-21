#include "CControl_Handler.hh"
#include "Options.hh"
#include "MongoLog.hh"
#include <string>
#include <iostream>
#include <limits.h>
#include <unistd.h>
#include <chrono>
#include <csignal>
#include <atomic>

std::atomic_bool b_run = true;

void SignalHandler(int signum) {
    std::cout << "Received signal "<<signum<<std::endl;
    b_run = false;
    return;
}

#include <mongocxx/instance.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>

int main(int argc, char** argv){

  mongocxx::instance instance{};
  // Parse arguments
  if(argc < 3){
    std::cout<<"USAGE: ./ccontrol {ID} {MONGO_URI}"<<std::endl;
    std::cout<<"Where MONGO_URI is the URI of the command DB"<<std::endl;
    return -1;
  }
  std::string dbname = "daq";
  if(argc >=3)
    dbname = argv[3];

  // Control and status DB connectivity
  // We're going to poll control for commands
  // and update status with a heartbeat
  std::string mongo_uri = argv[2];
  mongocxx::uri uri(mongo_uri.c_str());
  mongocxx::client client(uri);
  mongocxx::database db = client[dbname];
  mongocxx::collection control = db["control"];
  mongocxx::collection status = db["status"];
  mongocxx::collection options_collection = db["options"];
  mongocxx::collection dac_collection = db["dac_calibration"];

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
  int ret = logger->Initialize(mongo_uri, dbname, "log", hostname, true);
  if(ret!=0){
    std::cout<<"Log couldn't be initialized. Exiting."<<std::endl;
    exit(-1);
  }

  // Options
  Options *options = NULL;
  
  // Holds session data
  CControl_Handler *fHandler = new CControl_Handler(logger, hostname);
  using namespace std::chrono;

  while(b_run){

    auto order = bsoncxx::builder::stream::document{} <<
      "_id" << 1 <<bsoncxx::builder::stream::finalize;
    auto opts = mongocxx::options::find{};
    opts.sort(order.view());
    try {
      mongocxx::cursor cursor = control.find(
         bsoncxx::builder::stream::document{}<< "host" << hostname <<"acknowledged." + hostname <<
         bsoncxx::builder::stream::open_document << "$exists" << 0 <<
         bsoncxx::builder::stream::close_document << bsoncxx::builder::stream::finalize,
         opts
         );
      for (auto doc : cursor) {
        // Acknowledge the commands
        control.update_one(
            bsoncxx::builder::stream::document{} << "_id" << doc["_id"].get_oid() <<
            bsoncxx::builder::stream::finalize,
            bsoncxx::builder::stream::document{} << "$set" <<
            bsoncxx::builder::stream::open_document << "acknowledged." + hostname <<
            duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() <<
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
        } catch(const std::exception E){
          logger->Entry(MongoLog::Warning,
              "ccontrol: Received a document from the dispatcher missing [command|number]");
          continue;
        }

        // If the command is arm gonna use the options file to load the V2718, DDC10, etc...settings
        std::string mode = "";
        if(command == "arm"){
          try{
            mode = doc["mode"].get_utf8().value.to_string();
          } catch(const std::exception E){
            logger->Entry(MongoLog::Warning, "ccontrol: Received an arm document with no run mode");
            continue;
          }

          // Get an override doc from the 'options_override' field if it exists
          std::string override_json = "";
          try{
            bsoncxx::document::view oopts = doc["options_override"].get_document().view();
            override_json = bsoncxx::to_json(oopts);
          } catch(const std::exception E){
            logger->Entry(MongoLog::Local, "No override options provided");
          }

          //Here are our options
          if(options != NULL) {
            delete options;
            options = NULL;
          }
          options = new Options(logger, mode, hostname, mongo_uri, dbname, override_json);

          // Initialise the V2178, V1495 and DDC10...etc.
          if(fHandler->DeviceArm(run, options) != 0){
            logger->Entry(MongoLog::Warning, "Failed to initialize devices");
          }

        } // end if "arm" command
        else if(command == "start"){
          if((fHandler->DeviceStart()) != 0){
            logger->Entry(MongoLog::Warning, "Failed to start devices");
          }
        }
        else if(command == "stop"){
          if((fHandler->DeviceStop()) != 0){
            logger->Entry(MongoLog::Warning, "Failed to stop devices");
          }
        }
      } //end for
    } catch (const std::exception& e) {
      std::cout<<"Can't access db, I'll keep doing my thing\n";
      continue;
    }

    // Report back on what we are doing
    try{
      status.insert_one(fHandler->GetStatusDoc(hostname));
    } catch(const std::exception& e) {
      std::cout<<"Couldn't update database, I'll keep doing my thing\n";
    }
    std::this_thread::sleep_for(seconds(1));
  } // while run
  if (options != NULL) delete options;
  delete fHandler;
  delete logger;
  return 0;
}
