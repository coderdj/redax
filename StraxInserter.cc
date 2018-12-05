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
  fFirmwareVersion = -1;
}

StraxInserter::~StraxInserter(){  
}

int StraxInserter::Initialize(Options *options, MongoLog *log,  StraxFileHandler *handler,
			      DAQController *dataSource){
  fOptions = options;
  fChunkLength = fOptions->GetLongInt("strax_chunk_length", 20e9); // default 20s
  fChunkOverlap = fOptions->GetInt("strax_chunk_overlap", 5e8); // default 0.5s
  fFragmentLength = fOptions->GetInt("strax_fragment_length", 110*2);

  // To start we do not know which FW version we're dealing with (for data parsing)
  fFirmwareVersion = -1;

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


int StraxInserter::ParseDocuments(
				  std::map<std::string, std::string*> &strax_docs,
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
      u_int32_t event_size = buff[idx]&0xFFFF; // In bytes
      u_int32_t channel_mask = buff[idx+1]&0xFF; // Channels in event
      u_int32_t channels_in_event = __builtin_popcount(channel_mask);
      u_int32_t board_fail  = buff[idx+1]&0x4000000; //Board failed. Never saw this set.
      u_int32_t event_time = buff[idx+3]&0x7FFFFFFF;
      
      // I've never seen this happen but afraid to put it into the mongo log
      // since this call is in a loop
      if(board_fail==1)
	std::cout<<"Oh no your board failed"<<std::endl; //do something reasonable
      
      // If we don't know which firmware we're using, check now
      if(fFirmwareVersion == -1){
	DetermineDataFormat(&(buff[idx]), event_size, channels_in_event);
	if(fFirmwareVersion == 0)
	  std::cout<<"Detected XENON1T firmware"<<std::endl;
	else
	  std::cout<<"Detected stock firmware"<<std::endl;
      }

      idx += 4; // Skip the header
      
      for(unsigned int channel=0; channel<8; channel++){
	if(!((channel_mask>>channel)&1)) // Make sure channel in data
	  continue;

	u_int32_t channel_size = event_size / channels_in_event;
	u_int32_t channel_time = event_time;

	if(fFirmwareVersion == 0){
	  channel_size = buff[idx] - 2; // In words (4 bytes). The -2 is cause of header
	  idx++;
	  channel_time = buff[idx]&0x7FFFFFFF;
	  idx++;
	}

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
	u_int64_t fFullChunkLength = fChunkLength+fChunkOverlap;
	u_int64_t chunk_id = u_int64_t(Time64/fFullChunkLength);
	bool nextpre=false;//, prevpost=false;
	if(((chunk_id+1)*fFullChunkLength)-Time64 < fChunkOverlap)
	  nextpre=true;
	//if(Time64-(fFullChunkLength*chunk_id) < fChunkOverlap && chunk_id!=0)
	//  prevpost=true;

	// We're now at the first sample of the channel's waveform. This
	// will be beautiful. First we reinterpret the channel as 16
	// bit because we want to allow also odd numbers of samples
	// as FragmentLength
	u_int16_t *payload = reinterpret_cast<u_int16_t*>(buff);
	u_int32_t samples_in_channel = (channel_size)*2;
	u_int32_t index_in_sample = 0;
	u_int32_t offset = idx*2;
	u_int16_t fragment_index = 0;
	
	while(index_in_sample < samples_in_channel){
	  //char *fragment = new char[fFragmentLength + fStraxHeaderSize];
	  std::string fragment;
	  
	  // How long is this fragment?
	  u_int32_t max_sample = index_in_sample + fFragmentLength/2;
	  u_int32_t samples_this_channel = fFragmentLength/2;
	  if((unsigned int)(fFragmentLength/2 + (fragment_index*fFragmentLength/2)) >
	     samples_in_channel){
	    max_sample = index_in_sample + (samples_in_channel -
					    (fragment_index*fFragmentLength/2));
	    samples_this_channel = max_sample-index_in_sample;
	  }
	  

	  // Cast everything to char so we can put it in our buffer.
	  u_int16_t cl = u_int16_t(fOptions->GetChannel(dp.bid, channel));

	  // Failing to discern which channel we're getting data from seems serious enough to throw
	  if(cl==-1)
	    throw std::runtime_error("Failed to parse channel map. I'm gonna just kms now.");

	  char *channelLoc = reinterpret_cast<char*> (&cl);
	  fragment.append(channelLoc, 2);

	  u_int16_t sw = 10;
	  char *sampleWidth = reinterpret_cast<char*> (&sw);
	  fragment.append(sampleWidth, 2);
	  
	  u_int64_t time_this_fragment = Time64+((fFragmentLength/2)*10*fragment_index);
	  char *pulseTime = reinterpret_cast<char*> (&time_this_fragment);
	  fragment.append(pulseTime, 8);

	  //u_int32_t ft = fFragmentLength/2;
	  char *fragmenttime = reinterpret_cast<char*> (&samples_this_channel);
	  fragment.append(fragmenttime, 4);

	  u_int32_t tii0 = 0;
	  char *thisoneiszero = reinterpret_cast<char*>(&tii0);
	  fragment.append(thisoneiszero, 4);

	  char *samplesthischannel = reinterpret_cast<char*> (&samples_in_channel);
	  fragment.append(samplesthischannel, 4);

	  char *fragmentindex = reinterpret_cast<char*> (&fragment_index);
	  fragment.append(fragmentindex, 2);

	  char *anotherzero = reinterpret_cast<char*> (&tii0);
	  fragment.append(anotherzero, 4);

	  u_int8_t rl = 0;
	  char *reductionLevel = reinterpret_cast<char*> (&rl);
	  fragment.append(reductionLevel, 1);
	  
	  // Copy the raw buffer	  
	  if(samples_this_channel>fFragmentLength/2){
	    cout<<samples_this_channel<<"!"<<std::endl;
	    exit(-1);
	  }

	  const char *data_loc = reinterpret_cast<const char*>(&(payload[offset+index_in_sample]));
	  fragment.append(data_loc, samples_this_channel*2);
	  while(fragment.size()<fFragmentLength+fStraxHeaderSize)
	    fragment.append("0");	  

	  //copy(data_loc, data_loc+(samples_this_channel*2),&(fragment[31]));

	  
	  // Minor mess to maintain the same width of file names and do the pre/post stuff
	  // If not in pre/post
	  std::string chunk_index = std::to_string(chunk_id);
	  while(chunk_index.size() < fChunkNameLength)
	    chunk_index.insert(0, "0");

	  if(!nextpre){// && !prevpost){	      
	    if(strax_docs.find(chunk_index) == strax_docs.end())
	      strax_docs[chunk_index] = new std::string();	    
	    strax_docs[chunk_index]->append(fragment);
	    fragments_inserted++;
	  }
	  else{// if(nextpre){
	    std::string nextchunk_index = std::to_string(chunk_id+1);
	    while(nextchunk_index.size() < fChunkNameLength)
	      nextchunk_index.insert(0, "0");

	    if(strax_docs.find(nextchunk_index+"_pre") == strax_docs.end())
	      strax_docs[nextchunk_index+"_pre"] = new std::string();
	    strax_docs[nextchunk_index+"_pre"]->append(fragment);

	    if(strax_docs.find(chunk_index+"_post") == strax_docs.end())
	      strax_docs[chunk_index+"_post"] = new std::string();
	    strax_docs[chunk_index+"_post"]->append(fragment);
	  }
	  /*else if(prevpost){
	    std::string prevchunk_index = std::to_string(chunk_id-1);
	    while(prevchunk_index.size() < fChunkNameLength)
	      prevchunk_index.insert(0, "0");	    

	    if(strax_docs.find(prevchunk_index+"_post") == strax_docs.end())
	      strax_docs[prevchunk_index+"_post"] = new std::string();
	    strax_docs[prevchunk_index+"_post"]->append(fragment);

	    if(strax_docs.find(chunk_index+"_pre") == strax_docs.end())
              strax_docs[chunk_index+"_pre"] = new std::string();
            strax_docs[chunk_index+"_pre"]->append(fragment);

	  }
	  else
	    std::cout<<"Oh no! Where should this data go?"<<std::endl;
	  */
	  fragment_index++;
	  index_in_sample = max_sample;	  
	}
	// Go to next channel
	idx+=channel_size;
      }
    }
    else
      idx++;
  }
  
  //blosc_destroy();
  return fragments_inserted;
}


int StraxInserter::ReadAndInsertData(){
  
  std::vector <data_packet> *readVector=NULL;
  int read_length = fDataSource->GetData(readVector);
  std::map<std::string, std::string*> fragments; 
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

    usleep(10); // 10ms sleep
    read_length = fDataSource->GetData(readVector);
  }

  // At end of run insert whatever is left
  if(fragments.size()>0)
    fStraxHandler->InsertFragments(fragments);
  return 0;  
}

void StraxInserter::DetermineDataFormat(u_int32_t *buff, u_int32_t event_size,
					u_int16_t channels_in_event){
  /*
    This function should automatically sense which data format we're dealing with. 
    It does this by looking at various control words and trying to deduce from their
    values what this must be. We were unable to think of a 100% deterministic way to 
    say beyond any doubt which format this is, but we think the combination of circumstance
    required to fool this series of checks is so unlikely there is no realistic chance
    of choosing incorrectly.
    And if we do it will just seg fault, not explode.
   */

  // Start after header
  unsigned int idx = 4;
  
  for(unsigned int ch=0; ch<channels_in_event; ch++){
    u_int32_t channel_event_size = buff[idx];
    u_int32_t channel_time_tag = buff[idx+1];

    // Check 1: Would adding channel_event_size to idx go over size or event
    if(channel_event_size + idx > event_size){
      fFirmwareVersion = 1; // DEFAULT (no ZLE)
      return;
    }
    
    // Check 2: Our samples are 14-bit so if bits 14/15 or 30/31 of these words are
    // non-zero then this must be the DPP_XENON firmware
    if( (channel_time_tag>>14 != 0) || (channel_time_tag>>15 != 0) ||
	(channel_time_tag>>30 != 0) || (channel_time_tag>>31 != 0) ||
	(channel_event_size>>14 != 0) || (channel_event_size>>15 != 0) ||
	(channel_event_size>>30 != 0) || (channel_event_size>>31 != 0) ){
      fFirmwareVersion = 0;
      return;
    }

    idx += channel_event_size;            
  } // end for

  if(idx == event_size-1)
    fFirmwareVersion = 0;
  else
    fFirmwareVersion = 1;
  
  return;      
}
		    
