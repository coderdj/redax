#include "MongoInserter.hh"
#include "DAQController.hh"

MongoInserter::MongoInserter(){
  fOptions = NULL;
  fDataSource = NULL;
  fActive = true;
  fBulkInsertSize=100;
  fLog = NULL;
  fErrorBit = false;
}

MongoInserter::~MongoInserter(){
}

int MongoInserter::Initialize(Options *options, MongoLog *log,  DAQController *dataSource){
  fOptions = options;
  fBulkInsertSize = fOptions->GetInt("bulk_insert_size", 100);
  fDataSource = dataSource;
  fLog = log;
  fErrorBit = false;
  return 0;
}

void MongoInserter::Close(){
  fActive = false;
}

std::string MongoInserter::FormatString(const std::string format,
					const std::string pw){
  char s[ format.size() + pw.size() ]; /* whatever MAX_URL_SIZE may be */
  sprintf(s, format.c_str(), pw.c_str());
  return std::string(s);
}

void MongoInserter::ParseDocuments(
		    std::vector<bsoncxx::document::value> &doc_array,
		    data_packet dp){
  // Take a buffer and break it up into one document per channel
  // Put these documents into doc array

  // Unpack the things from the data packet
  u_int32_t bid = dp.bid;
  vector<u_int32_t> clock_counters;
  for(int i=0; i<8; i++)
    clock_counters.push_back(dp.clock_counter);
  vector<u_int32_t> last_times_seen = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
				       0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
  u_int32_t size = dp.size;
  u_int32_t *buff = dp.buff;
  
  
  u_int32_t idx = 0;
  while(idx < size/sizeof(u_int32_t) &&
	buff[idx] != 0xFFFFFFFF){
    // Loop through entire buffer until finished
    // 0xFFFFFFFF are used as padding it seems
    
    if(buff[idx]>>20 == 0xA00){ // Found a header, start parsing
      //u_int32_t event_size = buff[idx]&0xFFFF; // In bytes
      u_int32_t channel_mask = buff[idx+1]&0xFF; // Channels in event
      u_int32_t board_fail  = buff[idx+1]&0x4000000; //Board failed. Never saw this set.

      // I've never seen this happe but afraid to put it into the mongo log
      // since this call is in a loop
      if(board_fail==1)
	std::cout<<"Oh no your board failed"<<std::endl; //do something reasonable
      
      idx += 4; // Skip the header
      
      for(unsigned int channel=0; channel<8; channel++){
	if(!((channel_mask>>channel)&1)) // Make sure channel in data
	  continue;
	u_int32_t channel_size = buff[idx]; // In words (4 bytes)
	idx++;
	u_int32_t channel_time = buff[idx]&0x7FFFFFFF;
	idx++;

	// OK. Here's the logic for the clock reset, and I realize this is the
	// second place in the code where such weird logic is needed but that's it
	// First, on the first instance of a channel we gotta check if
	// the channel clock rolled over BEFORE this clock and adjust the counter
	if(channel_time > 15e8 && dp.header_time<5e8 &&
	   last_times_seen[channel] == 0xFFFFFFFF){
	  clock_counters[channel]--;
	}
	// Now check the opposite
	else if(channel_time <5e8 && dp.header_time > 15e8 &&
		last_times_seen[channel] == 0xFFFFFFFF){
	  clock_counters[channel]++;
	}

	// Now check if this time < last time (indicates rollover)
	if(channel_time < last_times_seen[channel] &&
	   last_times_seen[channel]!=0xFFFFFFFF)
	  clock_counters[channel]++;

	last_times_seen[channel] = channel_time;
	
	int iBitShift = 31;
	int64_t Time64 = ((unsigned long)clock_counters[channel] <<
			    iBitShift) + channel_time;
	
	u_int32_t *channel_payload = new u_int32_t[channel_size-2];

	// Sanity check. Fail hard if this fails.
	if(idx+channel_size-1 < size){
	  copy(&(buff[idx]),&(buff[idx+channel_size-2]), channel_payload);
	  
	  doc_array.push_back(bsoncxx::builder::stream::document{} <<
			      "module" << static_cast<int32_t>(bid) <<
			      "channel" << static_cast<int32_t>(channel) <<
			      "channel_time" << static_cast<int32_t>(channel_time) <<
			      "size" << static_cast<int32_t>((channel_size-2)*
							     sizeof(u_int32_t)) <<
			      "time" << Time64 <<
			      "data" << bsoncxx::types::b_binary {
				bsoncxx::binary_sub_type::k_binary,
				  static_cast<u_int32_t>((channel_size-2)*
							 sizeof(u_int32_t)),
				  reinterpret_cast<unsigned char*>(channel_payload)}
			      << bsoncxx::builder::stream::finalize);
	  
	  
	}
	else{
	  std::cout<<"FAIL HARD"<<std::endl;
	}
	idx+=channel_size-2;
      }
    }
    else
      idx++;
  }
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
  std::vector<bsoncxx::document::value> documents;
  
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
	/*coll.insert_one(bsoncxx::builder::stream::document{} <<
			"module" << (*readVector)[i].bid <<
			"channel" << 0 <<
			"size" << static_cast<int32_t>((*readVector)[i].size) <<
			"data" << bsoncxx::types::b_binary {
			    bsoncxx::binary_sub_type::k_binary,
			    static_cast<u_int32_t>((*readVector)[i].size),
			    reinterpret_cast<unsigned char*>((*readVector)[i].buff)}
			<< bsoncxx::builder::stream::finalize);
	*/
	// Bulk Inserts
	ParseDocuments(documents, (*readVector)[i]);

	if(documents.size()>fBulkInsertSize){
	  try{
	    coll.insert_many(documents);
	    documents.clear();
	  }
	  catch(const std::exception &e){
	    fLog->Entry("Failure to insert raw data into database. Quitting. Error follows.",
			MongoLog::Error);
	    fLog->Entry(e.what(), MongoLog::Error);
	    fErrorBit = true;
	    documents.clear();
	  }
	}
	
	//coll.insert_one(bsoncxx::builder::stream::document{} <<
	//		"test"<<"fucker"<<bsoncxx::builder::stream::finalize);
	delete[] (*readVector)[i].buff;
      }
      delete readVector;
      readVector=NULL;
    }
    if(documents.size()>0){
      coll.insert_many(documents);
      documents.clear();
    }
    usleep(10000); // 10ms sleep
    read_length = fDataSource->GetData(readVector);
  }
  return 0;  
}


		    
