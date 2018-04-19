#include <iostream>
#include <string>
#include <limits.h>
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
  
  // The DAQController object is responsible for passing commands to the
  // boards and tracking the status
  DAQController *controller = new DAQController();  
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
	std::cout << "ERROR MALFORMED COMMAND: " << bsoncxx::to_json(doc) << "\n";
      }

      
      // Process commands
      if(command == "start"){

	if(controller->status() == 2)	  
	  controller->Start();       
	else
	  std::cout<<"Cannot start DAQ if not in armed state"<<std::endl;
      }
      else if(command == "stop"){
	// "stop" is also a general reset command and can be called any time
	std::cout<<"Run stop not implemented"<<std::endl;
	controller->Stop();
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
	    std::cout<<"option_name: "<<option_name<<std::endl;
	    
	    bsoncxx::stdx::optional<bsoncxx::document::value> trydoc =
	      options_collection.find_one(bsoncxx::builder::stream::document{}<<
					  "name" << option_name.c_str() <<
					  bsoncxx::builder::stream::finalize);
	    if(trydoc){
	      options = bsoncxx::to_json(*trydoc);
	      if(controller->InitializeElectronics(options)!=0){
		std::cout<<"Failed to initialize electronics"<<std::endl;
	      }
	      else{
		std::cout<<"Initialized electronics"<<std::endl;
	      }
	      
	      if(readoutThread!=NULL){
		std::cout<<"Can't start DAQ with active readout thread. Reset first."<<std::endl;
	      }
	      else
		readoutThread = new std::thread(DAQController::ReadThreadWrapper,
						(static_cast<void*>(controller))); 
	    }	  
	  }
	  catch( const std::exception &e){
	    std::cout<<"Want to arm boards but no valid mode provided"<<std::endl;
	    options = "";	  
	  }
	}
	else
	  std::cout<<"Cannot ARM DAQ when it's in 'running' or 'error' state."<<std::endl;
      }


      // This command was processed (or failed) so remove it
      control.delete_one(bsoncxx::builder::stream::document{} << "_id" <<
			 doc["_id"].get_oid() << bsoncxx::builder::stream::finalize);
    }

    // No need to hammer DB
    status.insert_one(bsoncxx::builder::stream::document{} << "host" << hostname
		      << "rate" << controller->data_rate() << "status" << controller->status()
		      << "buffer_length" << controller->buffer_length() <<
		      bsoncxx::builder::stream::finalize);
    usleep(1000000);
  }
  
  exit(0);
  

}



