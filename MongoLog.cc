#include "MongoLog.hh"

MongoLog::MongoLog(){
  fLogLevel = 0;
  fHostname = "_host_not_set";
}
MongoLog::~MongoLog(){};

int  MongoLog::Initialize(std::string connection_string,
			  std::string db, std::string collection,
			  std::string host, std::string dac_collection,
			  bool debug){
  try{
    mongocxx::uri uri{connection_string};
    fMongoClient = mongocxx::client(uri);
    fMongoCollection = fMongoClient[db][collection];
    if(dac_collection != "")
      fDAC_collection = fMongoClient[db][dac_collection];
  }
  catch(const std::exception &e){
    std::cout<<"Couldn't initialize the log. So gonna fail then."<<std::endl;
    return -1;
  }

  fHostname = host;
  
  if(debug)
    fLogLevel = 1;
  else
    fLogLevel = 0;

  return 0;
}

int MongoLog::Entry(std::string message, int priority){

  if(priority >= fLogLevel){
    try{
      fMongoCollection.insert_one(bsoncxx::builder::stream::document{} <<
				  "user" << fHostname <<
				  "message" << message <<
				  "priority" << priority <<
				  bsoncxx::builder::stream::finalize);
      std::cout<<"("<<priority<<"): "<<message<<std::endl;
    }
    catch(const std::exception &e){
      std::cout<<"Failed to insert log message "<<message<<" ("<<
	priority<<")"<<std::endl;
      return -1;
    }
  }
  return 0;
}

void MongoLog::UpdateDACDatabase(std::string run_identifier,
				 std::map<int,
				 std::vector<u_int16_t>>dac_values){
  using namespace bsoncxx::builder::stream;
  auto search_doc = document{} << "run" <<  run_identifier << finalize;
  auto update_doc = document{};
  update_doc<< "$set" << open_document << "run" << run_identifier;
  for(auto iter : dac_values){
    update_doc << std::to_string(iter.first) << open_array <<
      [&](array_context<> arr){
      for (u_int32_t i = 0; i < iter.second.size(); i++)
	arr << iter.second[i];
    } << close_array;
  }
  update_doc<<close_document;
  auto write_doc = update_doc<<finalize;
  mongocxx::options::update options;
  options.upsert(true);
  fDAC_collection.update_one(search_doc.view(), write_doc.view(), options);

}

int MongoLog::GetDACValues(int bid, int reference_run,
			   std::vector<u_int16_t> &dac_values){
  auto doc = fDAC_collection.find_one(bsoncxx::builder::stream::document{}<<
				      "run" << reference_run <<
				      bsoncxx::builder::stream::finalize);
  if(!doc)
    return -1;

  try{
    dac_values.clear();
    bsoncxx::array::view channel_arr = (*doc).view()[std::to_string(bid)].get_array().value;
    for(bsoncxx::array::element ele : channel_arr)
      dac_values.push_back(ele.get_int32());
    return 0;
  }catch(const std::exception &e){
    dac_values = std::vector<u_int16_t>(8, 4000);
  }
  return -1;
}
