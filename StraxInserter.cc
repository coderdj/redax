#include "StraxInserter.hh"
#include "DAQController.hh"

StraxInserter::StraxInserter(){
  fOptions = NULL;
  fDataSource = NULL;
  fActive = true;
  fChunkLength=0x7fffffff; // DAQ magic number
  fChunkNameLength=6;
  fChunkOverlap = 0x2FAF080;
  fFragmentLength=110*2;
  fStraxHeaderSize=31;
  fLog = NULL;
  fStraxHandler=NULL;
  fErrorBit = false;  
}

StraxInserter::~StraxInserter(){  
}

int StraxInserter::Initialize(Options *options, MongoLog *log,  StraxFileHandler *handler,
			      DAQController *dataSource){
  fOptions = options;
  fChunkLength = fOptions->GetLongInt("strax_chunk_length", 20e9); // default 20s
  fChunkOverlap = fOptions->GetInt("strax_chunk_overlap", 5e8); // default 0.5s
  fFragmentLength = fOptions->GetInt("strax_fragment_length", 110*2);  
  fDataSource = dataSource;
  fLog = log;
  fErrorBit = false;
  fStraxHandler = handler;

  std::cout<<"Strax output initialized with "<<fChunkLength<<" ns chunks and "<<
    fChunkOverlap<<" ns overlap time. Fragments are "<<fFragmentLength<<" bytes."<<std::endl;
  return 0;
}

void StraxInserter::Close(){
  fActive = false;
}

//  return u_int32_t(timestamp/fChunkLength);  


int StraxInserter::ParseDocuments(
				  std::map<std::string, std::vector<unsigned char*>> &strax_docs,
				  data_packet dp				   
				  ){
  // Take a buffer and break it up into one document per channel
  int fragments_inserted = 0;
  
  // Unpack the things from the data packet
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

      // I've never seen this happen but afraid to put it into the mongo log
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
	int64_t Time64 = 10*(((unsigned long)clock_counters[channel] <<
			      iBitShift) + channel_time); // in ns

	// Get the CHUNK and decide if this event also goes into a PRE/POST file
	u_int32_t chunk_id = u_int32_t(Time64/fChunkLength);
	bool pre=false, post=false;
	// Here PRE means PRE this one and POST the previous one
	if(Time64-(chunk_id*fChunkLength) < fChunkOverlap && chunk_id != 0)
	  pre=true;
	// Here POST means POST this one and PRE the next one
	if( ( (chunk_id+1)*fChunkLength )-Time64 < fChunkOverlap)
	  post=true;

	// We're now at the first sample of the channel's waveform. This
	// will be beautiful. First we reinterpret the channel as 16
	// bit because we want to allow also odd numbers of samples
	// as FragmentLength
	u_int16_t *payload = reinterpret_cast<u_int16_t*>(buff);
	u_int32_t samples_in_channel = channel_size*2;
	u_int32_t index_in_sample = 0;
	u_int32_t offset = idx*2;
	u_int16_t fragment_index = 0;
	
	while(index_in_sample < samples_in_channel){
	  unsigned char *fragment = new unsigned char[fFragmentLength + fStraxHeaderSize];

	  // How long is this fragment?
	  u_int32_t max_sample = index_in_sample + fFragmentLength/2;
	  u_int32_t samples_this_channel = fFragmentLength/2;
	  if((unsigned int)(fFragmentLength/2 + (fragment_index*fFragmentLength/2)) >
	     samples_in_channel){
	    max_sample = index_in_sample + (samples_in_channel -
					    (fragment_index*fFragmentLength/2));
	    samples_this_channel = max_sample-index_in_sample;
	  }
	  
	  
	  // Ugh. Types.
	  u_int16_t *channelLoc = reinterpret_cast<u_int16_t*>(&fragment[0]);
	  *channelLoc = u_int16_t(fOptions->GetChannel(dp.bid, channel));
	  u_int16_t *sampleWidth = reinterpret_cast<u_int16_t*>(&fragment[2]);
	  *sampleWidth = 10;
	  u_int64_t *pulsetime = reinterpret_cast<u_int64_t*>(&fragment[4]);
	  *pulsetime = u_int64_t(Time64);
	  u_int32_t *fragmenttime = reinterpret_cast<u_int32_t*>(&fragment[12]);
	  *fragmenttime = (fFragmentLength/2);
	  u_int32_t *somethingzero = reinterpret_cast<u_int32_t*>(&fragment[16]);
	  *somethingzero = 0;
	  u_int32_t *samplesthischannel = reinterpret_cast<u_int32_t*>(&fragment[20]);
	  *samplesthischannel = u_int32_t(samples_this_channel);
	  u_int16_t *fragmentindex = reinterpret_cast<u_int16_t*>(&fragment[24]);
	  *fragmentindex = u_int16_t(fragment_index);
	  u_int32_t *anotherzero = reinterpret_cast<u_int32_t*>(&fragment[26]);
	  *anotherzero = 0;
	  u_int8_t *reductionLevel = reinterpret_cast<u_int8_t*>(&fragment[30]);
	  *reductionLevel=0;

	  // Copy the raw buffer	  
	  if(samples_this_channel>fFragmentLength/2){
	    cout<<samples_this_channel<<"!"<<std::endl;
	    exit(-1);
	  }
	  const char *data_loc = reinterpret_cast<const char*>(&(payload[offset+index_in_sample]));
	  copy(data_loc, data_loc+(samples_this_channel*2),&(fragment[31]));

	  // Minor mess to maintain the same width of file names and do the pre/post stuff
	  std::string chunk_index = std::to_string(chunk_id);
	  while(chunk_index.size() < fChunkNameLength)
	    chunk_index.insert(0, "0");
	  strax_docs[chunk_index].push_back(fragment);
	  fragments_inserted++;
	  if(pre){
	    unsigned char *pf0 = new unsigned char[fFragmentLength + fStraxHeaderSize];
	    unsigned char *pf1 = new unsigned char[fFragmentLength + fStraxHeaderSize];
	    copy(fragment, fragment+(fFragmentLength+fStraxHeaderSize),&(pf0[0]));
	    copy(fragment, fragment+(fFragmentLength+fStraxHeaderSize),&(pf1[0]));
	    strax_docs[chunk_index+"_pre"].push_back(pf0);
	    std::string prechunk_index = std::to_string(chunk_id-1);
	    while(prechunk_index.size() < fChunkNameLength)
	      prechunk_index.insert(0, "0");
	    strax_docs[prechunk_index+"_post"].push_back(pf1);
	  }
	  if(post){
	    unsigned char *pf0 = new unsigned char[fFragmentLength + fStraxHeaderSize];
            unsigned char *pf1 = new unsigned char[fFragmentLength + fStraxHeaderSize];
            copy(fragment, fragment+(fFragmentLength+fStraxHeaderSize),&(pf0[0]));
            copy(fragment, fragment+(fFragmentLength+fStraxHeaderSize),&(pf1[0]));
	    strax_docs[chunk_index+"_post"].push_back(pf0);
	    std::string postchunk_index = std::to_string(chunk_id+1);
	    while(postchunk_index.size() < fChunkNameLength)
	      postchunk_index.insert(0, "0");
	    strax_docs[postchunk_index+"_pre"].push_back(pf1);
	  }

	  
	  fragment_index++;
	  index_in_sample = max_sample;	  
	}
	// Go to next channel
	idx+=channel_size-2;
      }
    }
    else
      idx++;
  }
  return fragments_inserted;
}


int StraxInserter::ReadAndInsertData(){
  
  std::vector <data_packet> *readVector=NULL;
  int read_length = fDataSource->GetData(readVector);
  std::map<std::string, std::vector<unsigned char*>> fragments;
  int buffered_fragments = 0;
  
  while(fActive || read_length>0){
    //std::cout<<"Factive: "<<fActive<<" read length: "<<read_length<<std::endl;
    if(readVector != NULL){
      for(unsigned int i=0; i<readVector->size(); i++){
	buffered_fragments += ParseDocuments(fragments, (*readVector)[i]);
	delete[] (*readVector)[i].buff;
      }
      delete readVector;
      readVector=NULL;
    }
    if(buffered_fragments>1000){
      fStraxHandler->InsertFragments(fragments);
      fragments.clear();
      buffered_fragments=0;
    }

    //usleep(10000); // 10ms sleep
    read_length = fDataSource->GetData(readVector);
  }

  // At end of run insert whatever is left
  if(fragments.size()>0)
    fStraxHandler->InsertFragments(fragments);
  return 0;  
}


		    
