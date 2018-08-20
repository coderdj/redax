#include <iostream>
#include <vector>
#include <string>

#include "MongoLog.hh"


int main(int argc, char** argv){
  
  // We use this code like so:
  // ./ccontrol {MONGO_URI} {LINK} {DETECTOR} {DETECTOR} ...
  // where MONGO_URI denotes the dax command DB URI
  // and {DETECTOR} is an arbitrarily long list of
  // detectors for which this crate controller is
  // responsible

  mongocxx::instance instance{};

  // Parse arguments
  if(argc < 4){
    std::cout<<"USAGE: ./ccontrol {MONGO_URI} {LINK} {DETECTOR_0}"<<
      " {DETECTOR_1} ... {DETECTOR_N}"<<std::endl;
    std::cout<<"Where MONGO_URI is the URI of the command DB"<<
      " and the DETECTOR is the detectors for which this"<<
      " crate controller is responsible."<<std::endl;
    return -1;
  }

  // Control and status DB connectivity
  // We're going to poll control for commands
  // and update status with a heartbeat
  std::string mongo_uri = argv[1];
  mongocxx::uri uri(mongo_uri.c_str());
  mongocxx::client client(uri);
  mongocxx::database db = client["dax"];
  mongocxx::collection control = db["control"];
  mongocxx::collection status = db["status"];

  // Optical link ID where crate controller connected
  // following CAEN numbering convention. N.B.! 
  // CAEN links are thread/process safe only on a link by link
  // basis (i.e. each link only touched by one process), so don't
  // try to share links with readers on the same machine.
  int optical_link = std::stoi(argv[2]);
    
  std::vector<std::string> detectors;
  for(int x=3; x<argc; x++)
    detectors.push_back(argv[x]);

  std::cout<<"Crate controller registered with "<<detectors.size()<<
    " detectors with V2718 on link "<<optical_link<<std::endl;

  // Logging
  MongoLog *logger = new MongoLog();
  int ret = logger->Initialize(mongo_uri, "dax", "log", true, "_ccontroller");
  if(ret!=0){
    std::cout<<"Log couldn't be initialized. Exiting."<<std::endl;
    exit(-1);
  }

  while(1){

    // Get documents with either "Start" or "Stop" commands
    bsoncxx::cursor cursor = control.find
      (
       bsoncxx::builder::stream::document{}<< "command" <<
       bsoncxx::builder::stream::open_document << "$in" <<
       "start" << "stop" << bsoncxx::builder::stream::close_array <<
       bsoncxx::builder::stream::close_document  <<
       "acknowledged" << bsoncxx::builder::stream::open_document << "$nin" <<
       bsoncxx::builder::stream::open_array << hostname <<
       bsoncxx::builder::stream::close_array	<<
       bsoncxx::builder::stream::close_document << bsoncxx::builder::stream::finalize
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

      std::cout<<"Found start command"<<std::endl;
    }
  }
  
  return 0;
}
