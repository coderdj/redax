#include "StraxInserter.hh"
#include "DAQController.hh"

StraxInserter::StraxInserter(){
  fOptions = NULL;
  fDataSource = NULL;
  fActive = true;
  fChunkLength=0x7fffffff; // DAQ magic number
  fFragmentLength=110*2;
  fStraxHeaderSize=31;
  fLog = NULL;
  fErrorBit = false;  
}

StraxInserter::~StraxInserter(){
}

int StraxInserter::Initialize(Options *options, MongoLog *log,  DAQController *dataSource){
  fOptions = options;
  fChunkLength = fOptions->GetInt("strax_chunk_length", 0x7fffffff);
  fFragmentLength = fOptions->GetInt("strax_fragment_length", 110*2);
  fDataSource = dataSource;
  fLog = log;
  fErrorBit = false;
  return 0;
}

void StraxInserter::Close(){
  fActive = false;
}

//  return u_int32_t(timestamp/fChunkLength);  


void StraxInserter::ParseDocuments(
				   std::map<u_int32_t, std::vector<unsigned char*>> &strax_docs,
				   data_packet dp){
  // Take a buffer and break it up into one document per channel
  // Put these documents into doc array

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
	u_int32_t chunk_id = u_int32_t(Time64/fChunkLength); 

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
	  if(fFragmentLength/2 + (fragment_index*fFragmentLength/2) > samples_in_channel){
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

	  strax_docs[chunk_id].push_back(fragment);
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
}


int StraxInserter::ReadAndInsertData(){
  
  std::vector <data_packet> *readVector=NULL;
  int read_length = fDataSource->GetData(readVector);
  std::map<u_int32_t, std::vector<unsigned char*>> fragments;
  
  while(fActive || read_length>0){
    if(readVector != NULL){
      for(unsigned int i=0; i<readVector->size(); i++){	
	ParseDocuments(fragments, (*readVector)[i]);		
	delete[] (*readVector)[i].buff;
      }
      delete readVector;
      readVector=NULL;
    }
    if(fragments.size()>0){
      //INSERT FRAGMENTS BUT RIGHT NOW I'M GONNA DELETE THEM
      
      for(auto const& chunk : fragments){
	auto fragments_this_chunk = chunk.second;
	for(unsigned int i=0; i<fragments_this_chunk.size(); i++){

	  if(i==0){
	    u_int32_t *bindata = reinterpret_cast<u_int32_t*>((fragments_this_chunk)[i]);
	    std::cout<<"BINDATA DUMP"<<std::endl;
	    for(unsigned int x=0; x<(fFragmentLength+fStraxHeaderSize)/4; x++){
	      std::cout<<hex<<bindata[x]<<std::endl;
	    }
	    std::cout<<"OVER"<<std::endl;
	  }
	 	    //for(unsigned int j=0; j<fChunkLength; j++){
	    //if(j>50)
	    //break;
	    //std::cout<<hex<<fragments_this_chunk[i]<<std::endl;
	    //}
	    delete[] fragments_this_chunk[i];
	  
	}
      }
      fragments.clear();
    }
    usleep(10000); // 10ms sleep
    read_length = fDataSource->GetData(readVector);
  }
  return 0;  
}


		    
