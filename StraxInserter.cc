#include <lz4.h>
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
  fErrorBit = false;
  fFirmwareVersion = -1;
  fMissingVerified = 0;
  fOutputPath = "";
  fChunkNameLength = 6;
}

StraxInserter::~StraxInserter(){  
}

int StraxInserter::Initialize(Options *options, MongoLog *log, DAQController *dataSource,
			      std::string hostname){
  fOptions = options;
  fChunkLength = fOptions->GetLongInt("strax_chunk_length", 20e9); // default 20s
  fChunkOverlap = fOptions->GetInt("strax_chunk_overlap", 5e8); // default 0.5s
  fFragmentLength = fOptions->GetInt("strax_fragment_length", 110*2);
  fCompressor = fOptions->GetString("compressor", "blosc");
  fHostname = hostname;
  std::string run_name = fOptions->GetString("run_identifier", "run");
  
  // To start we do not know which FW version we're dealing with (for data parsing)
  fFirmwareVersion = fOptions->GetInt("firmware_version", -1);
  if(fFirmwareVersion == -1){
	cout<<"Firmware version unspecified in options"<<endl;
	return -1;
  }
  if((fFirmwareVersion != 0) && (fFirmwareVersion != 1)){
	cout<<"Firmware version unidentified, accepted versions are {0, 1}"<<endl;
	return -1;
  }

  fMissingVerified = 0;
  fDataSource = dataSource;
  fLog = log;
  fErrorBit = false;

  std::string output_path = fOptions->GetString("strax_output_path", "./");
  try{    
    std::experimental::filesystem::path op(output_path);
    op /= run_name;
    fOutputPath = op;
    std::experimental::filesystem::create_directory(op);
    return 0;
  }
  catch(...){
    fLog->Entry(MongoLog::Error, "StraxInserter::Initialize tried to create output directory but failed. Check that you have permission to write here.");
    return -1;
  }
  std::cout<<"Strax output initialized with "<<fChunkLength<<" ns chunks and "<<
    fChunkOverlap<<" ns overlap time. Fragments are "<<fFragmentLength<<" bytes."<<std::endl;

  return 0;
}

void StraxInserter::Close(){
  fActive = false;

}


void StraxInserter::ParseDocuments(data_packet dp){
  
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
  int smallest_latest_index_seen = -1;
  
  u_int32_t idx = 0;
  while(idx < size/sizeof(u_int32_t) &&
	buff[idx] != 0xFFFFFFFF){
    // Loop through entire buffer until finished
    // 0xFFFFFFFF are used as padding it seems
    
    if(buff[idx]>>20 == 0xA00){ // Found a header, start parsing
      u_int32_t event_size = buff[idx]&0xFFFFFFF; // In bytes
      u_int32_t channel_mask = buff[idx+1]&0xFF; // Channels in event
      u_int32_t channels_in_event = __builtin_popcount(channel_mask);
      u_int32_t board_fail  = buff[idx+1]&0x4000000; //Board failed. Never saw this set.
      u_int32_t event_time = buff[idx+3]&0x7FFFFFFF;
      
      // I've never seen this happen but afraid to put it into the mongo log
      // since this call is in a loop
      if(board_fail==1)
	std::cout<<"Oh no your board failed"<<std::endl; //do something reasonable

      idx += 4; // Skip the header

      for(unsigned int channel=0; channel<8; channel++){
	if(!((channel_mask>>channel)&1)) // Make sure channel in data
	  continue;

	u_int32_t channel_size = (event_size - 4) / channels_in_event;
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
	   last_times_seen[channel] == 0xFFFFFFFF && clock_counters[channel]!=0){
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
	if(chunk_id > 10000){
	  std::cout<<"Chunk ID: "<<chunk_id<<" from time: "<<Time64<<" and length: "<<
	    fFullChunkLength<<" with channel time: "<<channel_time<<" and reset counter: "<<
	    clock_counters[channel]<<std::endl;
	  throw(std::runtime_error("Exception in clock times"));
	}
	// Check if this is the smallest_latest_index_seen
	if(smallest_latest_index_seen == -1 || int(chunk_id) < smallest_latest_index_seen)
	  smallest_latest_index_seen = chunk_id;
	
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
	    if(fFragments.find(chunk_index) == fFragments.end()){
	      fFragments[chunk_index] = new std::string();
	    }
	    fFragments[chunk_index]->append(fragment);
	    fragments_inserted++;
	  }
	  else{// if(nextpre){
	    std::string nextchunk_index = std::to_string(chunk_id+1);
	    while(nextchunk_index.size() < fChunkNameLength)
	      nextchunk_index.insert(0, "0");

	    if(fFragments.find(nextchunk_index+"_pre") == fFragments.end()){
	      fFragments[nextchunk_index+"_pre"] = new std::string();
	    }
	    fFragments[nextchunk_index+"_pre"]->append(fragment);

	    if(fFragments.find(chunk_index+"_post") == fFragments.end()){
	      fFragments[chunk_index+"_post"] = new std::string();
	    }
	    fFragments[chunk_index+"_post"]->append(fragment);
	  }
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
  if(smallest_latest_index_seen != -1)
    WriteOutFiles(smallest_latest_index_seen);
}


int StraxInserter::ReadAndInsertData(){
  
  std::vector <data_packet> *readVector=NULL;
  int read_length = fDataSource->GetData(readVector);  
  fActive = true;
  bool haddata=false;
  while(fActive || read_length>0){
    //std::cout<<"Factive: "<<fActive<<" read length: "<<read_length<<std::endl;
    if(readVector != NULL){
      haddata=true;
      for(unsigned int i=0; i<readVector->size(); i++){
	ParseDocuments((*readVector)[i]);
	delete[] (*readVector)[i].buff;
      }
      delete readVector;
      readVector=NULL;
    }    

    usleep(10); // 10ms sleep
    read_length = fDataSource->GetData(readVector);
  }

  if(haddata)
    WriteOutFiles(1000000, true);
  return 0;  
}


void StraxInserter::WriteOutFiles(int smallest_index_seen, bool end){
  // Write the contents of fFragments to blosc-compressed files

  std::map<std::string, std::string*>::iterator iter;
  for(iter=fFragments.begin();
      iter!=fFragments.end(); iter++){
    std::string chunk_index = iter->first;
    std::string idnr = chunk_index.substr(0, fChunkNameLength);
    int idnrint = std::stoi(idnr);
    if(!(idnrint < smallest_index_seen-1 || end))    
      continue;
    
    if(!std::experimental::filesystem::exists(GetDirectoryPath(chunk_index, true)))
      std::experimental::filesystem::create_directory(GetDirectoryPath(chunk_index, true));

    size_t uncompressed_size = iter->second->size();

    // blosc it
    char *out_buffer = NULL;
    int wsize = 0;
    if(fCompressor == "blosc"){
      out_buffer = new char[uncompressed_size+BLOSC_MAX_OVERHEAD];
      wsize = blosc_compress_ctx(5, 1, sizeof(char), uncompressed_size,  &((*iter->second)[0]),
				   out_buffer, uncompressed_size+BLOSC_MAX_OVERHEAD, "lz4", 0, 2);
    }
    else{
      size_t max_compressed_size = LZ4_compressBound(uncompressed_size);
      out_buffer = new char[max_compressed_size];
      wsize = LZ4_compress_default(&((*iter->second)[0]), out_buffer, uncompressed_size,
				   max_compressed_size);
    }
    // was using BLOSCLZ but it complained
    delete iter->second;
    
    std::ofstream writefile(GetFilePath(chunk_index, true), std::ios::binary);
    writefile.write(out_buffer, wsize);
    delete[] out_buffer;
    writefile.close();

    // Move this chunk from *_TEMP to the same path without TEMP
    if(!std::experimental::filesystem::exists(GetDirectoryPath(chunk_index, false)))
      std::experimental::filesystem::create_directory(GetDirectoryPath(chunk_index, false));
    std::experimental::filesystem::rename(GetFilePath(chunk_index, true),
					  GetFilePath(chunk_index, false));
    iter = fFragments.erase(iter);
    
    CreateMissing(idnrint);
    if(iter==fFragments.end())
      break;
  } // End for through fragments
  

  if(end){
    fFragments.clear();
    std::experimental::filesystem::path write_path(fOutputPath);
    std::string filename = fHostname;
    write_path /= "THE_END";
    if(!std::experimental::filesystem::exists(write_path)){
      std::cout<<"Creating END directory at "<<write_path<<std::endl;
      try{
	std::experimental::filesystem::create_directory(write_path);
      }
      catch(...){};
    }
    std::stringstream ss;
    ss<<std::this_thread::get_id();
    write_path /= fHostname + "_" + ss.str();
    std::ofstream outfile;
    outfile.open(write_path, std::ios::out);
    outfile<<"...my only friend";
    outfile.close();
  }

}

std::string StraxInserter::GetStringFormat(int id){
  std::string chunk_index = std::to_string(id);
  while(chunk_index.size() < fChunkNameLength)
    chunk_index.insert(0, "0");
  return chunk_index;
}

std::experimental::filesystem::path StraxInserter::GetDirectoryPath(std::string id,
								       bool temp){
  std::experimental::filesystem::path write_path(fOutputPath);
  write_path /= id;
  if(temp)
    write_path+="_temp";
  return write_path;
}

std::experimental::filesystem::path StraxInserter::GetFilePath(std::string id, bool temp){
  std::experimental::filesystem::path write_path = GetDirectoryPath(id, temp);
  std::string filename = fHostname;
  std::stringstream ss;
  ss<<std::this_thread::get_id();
  filename += "_";
  filename += ss.str();
  write_path /= filename;
  return write_path;
}

void StraxInserter::CreateMissing(u_int32_t back_from_id){

  for(unsigned int x=fMissingVerified; x<back_from_id; x++){
    std::string chunk_index = GetStringFormat(x);
    std::string chunk_index_pre = chunk_index+"_pre";
    std::string chunk_index_post = chunk_index+"_post";
    if(!std::experimental::filesystem::exists(GetFilePath(chunk_index, false))){
      if(!std::experimental::filesystem::exists(GetDirectoryPath(chunk_index, false)))
	std::experimental::filesystem::create_directory(GetDirectoryPath(chunk_index, false));
      std::ofstream o;
      o.open(GetFilePath(chunk_index, false));
      o.close();
    }
    if(x!=0 && !std::experimental::filesystem::exists(GetFilePath(chunk_index_pre, false))){
      if(!std::experimental::filesystem::exists(GetDirectoryPath(chunk_index_pre, false)))
	std::experimental::filesystem::create_directory(GetDirectoryPath(chunk_index_pre, false));
      std::ofstream o;
      o.open(GetFilePath(chunk_index_pre, false));
      o.close();
    }
    if(!std::experimental::filesystem::exists(GetFilePath(chunk_index_post, false))){
      if(!std::experimental::filesystem::exists(GetDirectoryPath(chunk_index_post, false)))
	std::experimental::filesystem::create_directory(GetDirectoryPath(chunk_index_post, false));
      std::ofstream o;
      o.open(GetFilePath(chunk_index_post, false));
      o.close();
    }
  }
  fMissingVerified = back_from_id;
}
