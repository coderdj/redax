#include "StraxFileHandler.hh"

StraxFileHandler::StraxFileHandler(MongoLog *log){
  fLog = log;
  fFullFragmentSize=0;
  fChunkNameLength = 6;
  fChunkCloseDelay = 10;
  fHostname = "reader";
  fCleanToId = 0;
}

StraxFileHandler::~StraxFileHandler(){
  End();
}

int StraxFileHandler::Initialize(std::string output_path, std::string run_name,
				 u_int32_t full_fragment_size, std::string hostname){

  fCleanToId = 0;
  
  // Clear any previous initialization
  End(true);

  fFullFragmentSize = full_fragment_size;
  std::cout<<"Defining full strax fragment size as "<<fFullFragmentSize<<std::endl;
  fRunName = run_name;
  fHostname = hostname;
  
  // "Did this random boost function make it into std yet?" Yes it did.
  try{
    std::experimental::filesystem::path op(output_path);
    op /= run_name;
    fOutputPath = op;
    std::experimental::filesystem::create_directory(op);    
    return 0;
  }
  catch(...){
    fLog->Entry("StraxFileHandler::Initialize tried to create output directory but failed."
		" Check that you have permission to write here.", MongoLog::Error);
  }
  
  return -1;
}

void StraxFileHandler::End(bool silent){

  // Loop through mutexes, lock them, and close each associated file
  std::cout<<"Closing open files"<<std::endl;
  //while(fFileMutexes.size()>0)
  if(!silent && (fFileHandles.size()>0 || fFileMutexes.size()>0))
    CleanUp(0, true);
  std::cout<<"Done closing open files."<<std::endl;
  fFileHandles.clear();
  fFileMutexes.clear();
  
}

std::string StraxFileHandler::GetStringFormat(int id){
  std::string chunk_index = std::to_string(id);
  while(chunk_index.size() < fChunkNameLength)
    chunk_index.insert(0, "0");
  return chunk_index;
}

int StraxFileHandler::InsertFragments(std::map<std::string, std::string*> &parsed_fragments){

  // Store the lowest ID that's been inserted this round
  int lowest_id = -1;
  
  for( auto const& idit : parsed_fragments ){

    // Here we insist that the 'id' string must begin with fChunkNameLength characters
    // that parse to an integer
    std::string id = idit.first;
    std::string idnr = id.substr(0, fChunkNameLength);
    int idnrint = std::stoi(idnr);
    if(idnrint < lowest_id || lowest_id == -1)
      lowest_id = idnrint;      
    
    // Create outfile and mutex pair in case they don't exist
    if( fFileMutexes.find(id) == fFileMutexes.end()){
      fFileMutexes[id].lock();
      std::experimental::filesystem::path write_path = GetDirectoryPath(id, true);//(fOutputPath);
      //write_path /= id;
      std::experimental::filesystem::create_directory(write_path);
      write_path = GetFilePath(id, true);
      fFileHandles[id].open(write_path, std::ios::out | std::ios::binary);
      fFileMutexes[id].unlock();
    }
    
    std::streamsize write_size = (std::streamsize)(idit.second->size());
    fFileMutexes[id].lock();    
    fFileHandles[id].write( &((*parsed_fragments[id])[0]), write_size);
    delete parsed_fragments[id];

    fFileMutexes[id].unlock();    
  }

  if(lowest_id > int(fChunkCloseDelay) && fChunkCloseMutex.try_lock()){
    CleanUp((u_int32_t)(lowest_id));
    fChunkCloseMutex.unlock();
  }

  return 0;
}

std::experimental::filesystem::path StraxFileHandler::GetDirectoryPath(std::string id,
								       bool temp){
  std::experimental::filesystem::path write_path(fOutputPath);
  write_path /= id;
  if(temp)
    write_path+="_temp";
  return write_path;
}

std::experimental::filesystem::path StraxFileHandler::GetFilePath(std::string id, bool temp){
  std::experimental::filesystem::path write_path = GetDirectoryPath(id, temp);
  
  std::string filename = fHostname;  
  write_path /= filename;
  return write_path;
}

void StraxFileHandler::CreateMissing(u_int32_t back_from_id){
  if(back_from_id < fChunkCloseDelay)
    return;
  for(unsigned int x=0; x<back_from_id-fChunkCloseDelay; x++){
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
    if(!std::experimental::filesystem::exists(GetFilePath(chunk_index_pre, false))){
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
      o.open(GetFilePath(chunk_index, false));
      o.close();
    }
  }
}

void StraxFileHandler::CleanUp(u_int32_t back_from_id, bool force_all){
  // We want to mark which files are definitely done writing and
  // should lose the TEMP marking so that the trigger can process
  // them. The only real way we know if a file is finished is if
  // it is at least 'n' id's back from the current file

  unsigned int largest_closed = 0;
  if(force_all)
    back_from_id = 1000000;
  
  // The variable fCleanToId holds the index to which we're sure we've written everything
  for(unsigned int index = fCleanToId; index < back_from_id - fChunkCloseDelay; index++){
    
    std::string chunk_index = GetStringFormat(index);
    std::string chunk_index_pre = chunk_index + "_pre";
    std::string chunk_index_post = chunk_index + "_post";

    if(fFileMutexes.find(chunk_index) != fFileMutexes.end()){
      if(fFileHandles[chunk_index].is_open()){
	if(index > largest_closed) largest_closed = index;
	fFileMutexes[chunk_index].lock();
	FinishFile(chunk_index);
	fFileMutexes[chunk_index].unlock();
      }
    }
    if(fFileMutexes.find(chunk_index_pre) != fFileMutexes.end()){
      if(fFileHandles[chunk_index_pre].is_open()){
	if(index > largest_closed) largest_closed = index;
	fFileMutexes[chunk_index_pre].lock();
	FinishFile(chunk_index_pre);
	fFileMutexes[chunk_index_pre].unlock();
      }
    }
    if(fFileMutexes.find(chunk_index_post) != fFileMutexes.end()){
      if(fFileHandles[chunk_index_post].is_open()){
	if(index > largest_closed) largest_closed = index;
	fFileMutexes[chunk_index_post].lock();
	FinishFile(chunk_index_post);
	fFileMutexes[chunk_index_post].unlock();
      }
    }

  }

  if(largest_closed != 0)
    CreateMissing(largest_closed);

  // At the end of the run we need to write "THE_END"
  if(force_all){
    std::experimental::filesystem::path write_path(fOutputPath);
    write_path /= "THE_END";
    if(!std::experimental::filesystem::exists(write_path)){
      std::cout<<"Creating END directory at "<<write_path<<std::endl;
      try{
        std::experimental::filesystem::create_directory(write_path);
      }
      catch(...){};
    }
    write_path /= fHostname;
    std::ofstream outfile;
    outfile.open(write_path, std::ios::out);
    outfile<<"...my only friend";
    outfile.close();
  
    // Prune _TEMP directories                                                                                                                                                                                     
    //for(auto& p: std::experimental::filesystem::directory_iterator(fOutputPath)){
    //std::string fss = p.path().string();
    //if(fss.substr( fss.length() - 5 ) == "_TEMP"){
    //try{
    //std::experimental::filesystem::remove(fss);
    //}catch(...){};
    //}
    //}

  }
}

void StraxFileHandler::FinishFile(std::string chunk_index){
  
  // Close this chunk!
  fFileHandles[chunk_index].close();

  // ZIP it up!
  std::ifstream ifs(GetFilePath(chunk_index, true), std::ios::binary);
  std::filebuf* pbuf = ifs.rdbuf();
  std::size_t size = pbuf->pubseekoff (0,ifs.end,ifs.in);
  pbuf->pubseekpos (0,ifs.in);

  // allocate memory to contain file data
  char *buffer = NULL;
  try{
    buffer=new char[size];
  }catch(const std::exception &e){
    std::cout<<"Can't make buffer of size "<<size<<std::endl;
    std::cout<<e.what();
    throw e;
  }

  // get file data
  pbuf->sgetn (buffer,size);

  // blosc it
  char *out_buffer = new char[size+BLOSC_MAX_OVERHEAD];
  int wsize = blosc_compress(5, 1, sizeof(float), size, buffer,
			     out_buffer, size+BLOSC_MAX_OVERHEAD);
  
  delete[] buffer;
  ifs.close();

  // I am afraid to stream to the 'finished' version so gonna remove the temp file,
  // then stream compressed data to it, then rename
  std::experimental::filesystem::remove(GetFilePath(chunk_index, true));
  std::ofstream writefile(GetFilePath(chunk_index, true), std::ios::binary);
  writefile.write(out_buffer, wsize);
  delete[] out_buffer;
  writefile.close();
  
  // Move this chunk from *_TEMP to the same path without TEMP
  if(!std::experimental::filesystem::exists(GetDirectoryPath(chunk_index, false)))
    std::experimental::filesystem::create_directory(GetDirectoryPath(chunk_index, false));
  std::experimental::filesystem::rename(GetFilePath(chunk_index, true),
					GetFilePath(chunk_index, false));
       
}
