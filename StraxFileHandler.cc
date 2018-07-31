#include "StraxFileHandler.hh"

StraxFileHandler::StraxFileHandler(MongoLog *log){
  fLog = log;
  fFullFragmentSize=0;
  fChunkNameLength = 6;
  fChunkCloseDelay = 5;
  fHostname = "reader";
}

StraxFileHandler::~StraxFileHandler(){
  End();
}

int StraxFileHandler::Initialize(std::string output_path, std::string run_name,
				 u_int32_t full_fragment_size, std::string hostname){

  // Clear any previous initialization
  End();
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

void StraxFileHandler::End(){

  // Loop through mutexes, lock them, and close each associated file
  std::cout<<"Closing open files"<<std::endl;
  while(fFileMutexes.size()>0)    
    CleanUp(0, true);
  std::cout<<"Done closing open files."<<std::endl;
  fFileHandles.clear();
  fFileMutexes.clear();
  
}

int StraxFileHandler::InsertFragments(std::map<std::string,
				      std::vector<char*> > parsed_fragments){

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
      std::experimental::filesystem::path write_path(fOutputPath);
      write_path /= id;
      std::experimental::filesystem::create_directory(write_path);
      write_path = GetFilePath(id, true);
      //std::cout<<"Seems that file doesn't exist, so opening. ID: "<<id<<
      //" Path: "<<write_path<<std::endl;
      fFileHandles[id].open(write_path, std::ios::out | std::ios::binary);
      fFileMutexes[id].unlock();
    }

    fFileMutexes[id].lock();
    //std::streamsize write_size = (std::streamsize)(idit.second.size()*fFullFragmentSize);
    //fFileHandles[id].write((parsed_fragments[id][0]), write_size);
    
    // END DELETE    
    for( unsigned int i=0; i<idit.second.size(); i++){
      fFileHandles[id].write(reinterpret_cast<const char*>(parsed_fragments[id][i]),
			     fFullFragmentSize);
      delete[] parsed_fragments[id][i];
    }

    fFileMutexes[id].unlock();    
  }

  if(lowest_id > 0)
    CleanUp((u_int32_t)(lowest_id));

  return 0;
}

std::experimental::filesystem::path StraxFileHandler::GetFilePath(std::string id, bool temp){
  std::experimental::filesystem::path write_path(fOutputPath);
  write_path /= id;
  std::string filename = fHostname;
  if(temp)
    filename+="_TEMP";
  write_path /= filename;
  return write_path;
}

void StraxFileHandler::CleanUp(u_int32_t back_from_id, bool force_all){
  // We want to mark which files are definitely done writing and
  // should lose the TEMP marking so that the trigger can process
  // them. The only real way we know if a file is finished is if
  // it is at least 'n' id's back from the current file
  for (auto const &mutex_itr : fFileMutexes){

    // Get the ID of this one
    std::string idnr = mutex_itr.first.substr(0, fChunkNameLength);
    u_int32_t idnrint = (u_int32_t)(std::stoi(idnr));
    if(force_all || (back_from_id > idnrint &&
		     back_from_id - idnrint > fChunkCloseDelay)){

      // try_lock cause if it is being used we can skip
      if(!fFileMutexes[mutex_itr.first].try_lock())
	continue;
      
      // Close this chunk!
      fFileHandles[mutex_itr.first].close();

      // Move this chunk from *_TEMP to the same path without TEMP
      std::experimental::filesystem::rename(GetFilePath(mutex_itr.first, true),
					    GetFilePath(mutex_itr.first, false));

      // Don't remove this mutex in this case, destroy the entries
      fFileHandles.erase(mutex_itr.first);
      fFileMutexes.erase(mutex_itr.first);
      continue;
    }

  }

  // If we call this with 'force_all' it means we're ending the run
  // so we need to put in the THE_END marker
  if(force_all){    
    std::experimental::filesystem::path write_path(fOutputPath);
    write_path /= "THE_END";
    std::cout<<"Creating END file at "<<write_path<<std::endl;
    std::experimental::filesystem::create_directory(write_path);
    write_path /= fHostname;
    std::ofstream outfile;
    outfile.open(write_path, std::ios::out);
    outfile<<"...my only friend";
    outfile.close();
  }
  
}
