#include "StraxFileHandler.hh"

StraxFileHandler::StraxFileHandler(MongoLog *log){
  fLog = log;
  fFullFragmentSize=0;
}

StraxFileHandler::~StraxFileHandler(){
  End();
}

int StraxFileHandler::Initialize(std::string output_path, std::string run_name,
				 u_int32_t full_fragment_size){

  // Clear any previous initialization
  End();
  fFullFragmentSize = full_fragment_size;
  fRunName = run_name;
  
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
  for( auto const& it : fFileMutexes ){
    auto id = it.first;
    fFileMutexes[id].lock();
    if(fFileHandles.find(id) == fFileHandles.end()){
      fLog->Entry("StraxFileHandler::End did not find mutex ID in file map.",
		  MongoLog::Warning);
      fFileMutexes[id].unlock();
      continue;
    }
    if(fFileHandles[id].is_open())
      fFileHandles[id].close();
    fFileMutexes[id].unlock();
  }
  fFileHandles.clear();
  fFileMutexes.clear();
  
}

int StraxFileHandler::InsertFragments(std::map<u_int32_t,
				      std::vector<unsigned char*> > parsed_fragments){

  for( auto const& [id, fragments] : parsed_fragments ){

    // Create outfile and mutex pair in case they don't exist
    if( fFileMutexes.find(id) == fFileMutexes.end() ){
      fFileMutexes[id].lock();
      std::experimental::filesystem::path write_path(fOutputPath);
      write_path /= (fRunName + "_" + std::to_string(id) + ".stx");
      fFileHandles[id].open(write_path, std::ios::out | std::ios::binary);
      fFileMutexes[id].unlock();
    }

    fFileMutexes[id].lock();
    for( unsigned int i=0; i<fragments.size(); i++){
      //std::cout<<"Writing "<<fFullFragmentSize<<" bytes"<<std::endl;
      fFileHandles[id].write(reinterpret_cast<const char*>(parsed_fragments[id][i]),
			     fFullFragmentSize);
      delete[] parsed_fragments[id][i];
    }

    fFileMutexes[id].unlock();    
  }

  // Now go through and close all files more than 'n' id's back (keep from
  // going too nuts with the open file handles)


  return 0;
}
