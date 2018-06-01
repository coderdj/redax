#include "MongoInserter.hh"
#include "DAQController.hh"

MongoInserter::MongoInserter(){
  fOptions = NULL;
  fDataSource = NULL;
  fActive = true;
}

MongoInserter::~MongoInserter(){
}

int MongoInserter::Initialize(Options *options, DAQController *dataSource){
  fOptions = options; 
  fDataSource = dataSource;
  return 0;
}

void MongoInserter::Close(){
  fActive = false;
}

std::string MongoInserter::FormatString(const std::string format, const std::string pw){
  char s[ format.size() + pw.size() ]; /* whatever MAX_URL_SIZE may be */
  sprintf(s, format.c_str(), pw.c_str());
  return std::string(s);
}


int MongoInserter::ReadAndInsertData(){

  // Parse the uri. Probably need some validation here
  std::string uri_base = fOptions->GetString("mongo_uri");
  std::cout<<"BASE: "<<uri_base<<std::endl;
  std::string pw = std::getenv("MONGO_PASSWORD");
  std::cout<<FormatString(uri_base, pw)<<std::endl;
  
  mongocxx::uri uri{FormatString(uri_base, pw)};
  mongocxx::client client{uri};

  std::string database, collection;
  try{
    database = fOptions->GetString("mongo_database");
    collection = fOptions->GetString("mongo_collection");
    assert(database!="");
    assert(collection!="");
  }
  catch(const std::exception &e){
    database = "data";
    collection = "test";
  }
  mongocxx::database db = client[database];
  mongocxx::collection coll = db[collection];
  
  
  std::vector <data_packet> *readVector=NULL;
  int read_length = fDataSource->GetData(readVector);
  while(fActive || read_length>0){

    if(readVector != NULL){
      for(unsigned int i=0; i<readVector->size(); i++){	
	// Just delete
	//std::cout<<(*readVector)[i]->size<<std::endl;
	//for(unsigned int j=0; j<(*readVector)[i]->size/4; j++)
	//std::cout<<hex<<(*readVector)[i]->buff<<std::endl;

	// Put into Mongo. To do: bulk inserts!
	// Notes: size cast to int, apparently no uint in bson
	//        readVector cast to char* (used to want void* in old driver)
	//        binary size requires uint. k_binary sub type seems to be the applicable one
	coll.insert_one(bsoncxx::builder::stream::document{} <<
			"module" << (*readVector)[i].bid <<
			"channel" << 0 <<
			"size" << static_cast<int32_t>((*readVector)[i].size) <<
			"data" << bsoncxx::types::b_binary {
			    bsoncxx::binary_sub_type::k_binary,
			    static_cast<u_int32_t>((*readVector)[i].size),
			    reinterpret_cast<unsigned char*>((*readVector)[i].buff)}
			<< bsoncxx::builder::stream::finalize);
	
	//coll.insert_one(bsoncxx::builder::stream::document{} <<
	//		"test"<<"fucker"<<bsoncxx::builder::stream::finalize);
	delete[] (*readVector)[i].buff;
      }
      delete readVector;
      readVector=NULL;
    }
    usleep(10000); // 10ms sleep
    read_length = fDataSource->GetData(readVector);
  }
  return 0;  
}

