#include "StraxOutput.hh"
#include <lz4frame.h>
#include "DAQController.hh"
#include "MongoLog.hh"
#include "Options.hh"
#include <blosc.h>
#include <cstring>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace fs=std::experimental::filesystem;
using namespace std::chrono;

StraxOutput::StraxOutput(Option* options, MongoLog* log, std::string hostname){
  fChunkNameLength=6;
  fStraxHeaderSize=24;
  fThreadId = std::this_thread::get_id();
  fBytesProcessed = 0;
  fFragmentSize = 0;
  fForceQuit = false;
  fFragmentsProcessed = 0;
  fEventsProcessed = 0;
  
  fOptions = options;
  fChunkLength = long(fOptions->GetDouble("strax_chunk_length", 5)*1e9); // default 5s
  fChunkOverlap = long(fOptions->GetDouble("strax_chunk_overlap", 0.5)*1e9); // default 0.5s
  fFragmentBytes = fOptions->GetInt("strax_fragment_payload_bytes", 110*2);
  fCompressor = fOptions->GetString("compressor", "lz4");
  fFullChunkLength = fChunkLength+fChunkOverlap;
  fHostname = hostname;
  std::string run_name = fOptions->GetString("run_identifier", "run");

  fMissingVerified = 0;
  fLog = log;
  fErrorBit = false;

  fBufferNumChunks = fOptions->GetInt("strax_buffer_num_chunks", 2);
  fWarnIfChunkOlderThan = fOptions->GetInt("strax_chunk_phase_limit", 2);

  std::string output_path = fOptions->GetString("strax_output_path", "./");
  try{
    fs::path op(output_path);
    op /= run_name;
    fOutputPath = op;
    fs::create_directory(op);
  }
  catch(...){
    fLog->Entry(MongoLog::Error, "StraxOutput::Initialize tried to create output directory but failed. Check that you have permission to write here.");
    throw std::exception("Could not create output directory");
  }
}

StraxOutput::~StraxOutput(){
  fActive = false;
  int counter_short = 0, counter_long = 0;
  if (fBufferLength.load() > 0)
    fLog->Entry(MongoLog::Local, "Thread %lx waiting to stop, has %i events left",
        fThreadId, fBufferLength.load());
  else
    fLog->Entry(MongoLog::Local, "Thread %lx stopping", fThreadId);
  int events_start = fBufferLength.load();
  do{
    events_start = fBufferLength.load();
    while (fRunning && counter_short++ < 500)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (counter_short >= 500)
      fLog->Entry(MongoLog::Message, "Thread %lx taking a while to stop, still has %i evts",
          fThreadId, fBufferLength.load());
    counter_short = 0;
  } while (fRunning && fBufferLength.load() > 0 && events_start > fBufferLength.load() && counter_long++ < 10);
  if (fRunning) {
    fLog->Entry(MongoLog::Warning, "Force-quitting thread %lx: %i events lost",
        fThreadId, fBufferLength.load());
    fForceQuit = true;
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  while (fRunning) {
    fLog->Entry(MongoLog::Message, "Still waiting for thread %lx to stop", fThreadId);
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  long total_dps = std::accumulate(fBufferCounter.begin(), fBufferCounter.end(), 0,
      [&](long tot, auto& p){return tot + p.second;});
  std::map<std::string, long> counters {
    {"bytes", fBytesProcessed},
    {"fragments", fFragmentsProcessed},
    {"events", fEventsProcessed},
    {"data_packets", total_dps}};
  fOptions->SaveBenchmarks(counters, fBufferCounter,
      fProcTimeDP.count(), fProcTimeEv.count(), fProcTimeCh.count(), fCompTime.count());
}

int StraxOutput::Initialize(Options *options, MongoLog *log, DAQController *dataSource,
			      std::string hostname){
  return 0;
}


void StraxOutput::AddFragmentToBuffer(std::string& fragment, int64_t timestamp) {
  // Get the CHUNK and decide if this event also goes into a PRE/POST file
  int chunk_id = timestamp/fFullChunkLength;
  bool nextpre = (chunk_id+1)* fFullChunkLength - timestamp <= fChunkOverlap;
  // Minor mess to maintain the same width of file names and do the pre/post stuff
  // If not in pre/post
  std::string chunk_index = GetStringFormat(chunk_id);
  int min_chunk(0), max_chunk(1);
  const auto [min_chunk_, max_chunk_] = std::minmax_element(fFragments.begin(), fFragments.end(), 
      [&](auto& l, auto& r) {return std::stoi(l.first) < std::stoi(r.first);});
  if (fFragments.size() > 0) {
    min_chunk = std::stoi((*min_chunk_).first);
    max_chunk = std::stoi((*max_chunk_).first);
  }

  if (min_chunk - chunk_id > fWarnIfChunkOlderThan) {
    const short* channel = (const short*)(fragment.data()+14);
    fLog->Entry(MongoLog::Warning,
        "Thread %lx got data from channel %i that's %i chunks behind the buffer, it might get lost",
        fThreadId, *channel, min_chunk - chunk_id);
  } else if (chunk_id - max_chunk > 2) {
    fLog->Entry(MongoLog::Message, "Thread %lx skipped %i chunk(s)",
        fThreadId, chunk_id - max_chunk - 1);
  }

  fFragmentSize += fragment.size();

  if(!nextpre){
    if(fFragments.count(chunk_index) == 0){
      fFragments[chunk_index] = new std::string();
    }
    fFragments[chunk_index]->append(fragment);
    fTimeLastSeen[chunk_index] = system_clock::now();
  } else {
    std::string nextchunk_index = GetStringFormat(chunk_id+1);

    if(fFragments.count(nextchunk_index+"_pre") == 0){
      fFragments[nextchunk_index+"_pre"] = new std::string();
    }
    fFragments[nextchunk_index+"_pre"]->append(fragment);
    fTimeLastSeen[nextchunk_index+"_pre"] = system_clock::now();

    if(fFragments.count(chunk_index+"_post") == 0){
      fFragments[chunk_index+"_post"] = new std::string();
    }
    fFragments[chunk_index+"_post"]->append(fragment);
    fTimeLastSeen[chunk_index+"_post"] = system_clock::now();
  }
}

int StraxOutput::ReadAndInsertData(){
  fThreadId = std::this_thread::get_id();
  fActive = fRunning = true;
  fBufferLength = 0;
  std::chrono::microseconds sleep_time(10);
  if (fOptions->GetString("buffer_type", "dual") == "dual") {
    while(fActive == true){
      std::list<data_packet*> b;
      if (fDataSource->GetData(&b)) {
        fBufferLength = b.size();
        fBufferCounter[int(b.size())]++;
        for (auto& dp_ : b) {
          ProcessDatapacket(dp_);
          fBufferLength--;
          dp_ = nullptr;
          if (fForceQuit) break;
        }
        if (fForceQuit) for (auto& dp_ : b) if (dp_ != nullptr) delete dp_;
        b.clear();
        WriteOutFiles();
      } else {
        std::this_thread::sleep_for(sleep_time);
      }
    }
  } else {
    data_packet* dp;
    while (fActive == true) {
      if (fDataSource->GetData(dp)) {
        fBufferLength = 1;
        fBufferCounter[1]++;
        ProcessDatapacket(dp);
        fBufferLength = 0;
        WriteOutFiles();
      } else {
        std::this_thread::sleep_for(sleep_time);
      }
    }
  }
  if (fBytesProcessed > 0)
    WriteOutFiles(true);
  fRunning = false;
  return 0;
}

// Can tune here as needed, these are defaults from the LZ4 examples
static const LZ4F_preferences_t kPrefs = {
  { LZ4F_max256KB, LZ4F_blockLinked, LZ4F_noContentChecksum, LZ4F_frame, 0, { 0, 0 } },
    0,   /* compression level; 0 == default */
    0,   /* autoflush */
    { 0, 0, 0 },  /* reserved, must be set to 0 */
};

void StraxOutput::WriteOutFiles(bool end){
  // Write the contents of fFragments to compressed files
  system_clock::time_point comp_start, comp_end;
  std::vector<std::string> idx_to_clear;
  int max_chunk = -1;
  for (auto& iter : fFragments) max_chunk = std::max(max_chunk, std::stoi(iter.first));
  int write_lte = max_chunk - fBufferNumChunks;
  for (auto& iter : fFragments) {
    if (iter.first == "")
        continue; // not sure why, but this sometimes happens during bad shutdowns
    std::string chunk_index = iter.first;
    if (std::stoi(chunk_index) > write_lte && !end) continue;
    //fLog->Entry(MongoLog::Local, "Thread %lx max %i current %i buffer %i write_lte %i",
    //    fThreadId, max_chunk, std::stoi(chunk_index), fBufferNumChunks, write_lte);

    comp_start = system_clock::now();
    if(!fs::exists(GetDirectoryPath(chunk_index, true)))
      fs::create_directory(GetDirectoryPath(chunk_index, true));

    size_t uncompressed_size = iter.second->size();

    // Compress it
    char *out_buffer = NULL;
    int wsize = 0;
    if(fCompressor == "blosc"){
      out_buffer = new char[uncompressed_size+BLOSC_MAX_OVERHEAD];
      wsize = blosc_compress_ctx(5, 1, sizeof(char), uncompressed_size,  iter.second->data(),
				   out_buffer, uncompressed_size+BLOSC_MAX_OVERHEAD, "lz4", 0, 2);
    }
    else{
      // Note: the current package repo version for Ubuntu 18.04 (Oct 2019) is 1.7.1, which is
      // so old it is not tracked on the lz4 github. The API for frame compression has changed
      // just slightly in the meantime. So if you update and it breaks you'll have to tune at least
      // the LZ4F_preferences_t object to the new format.
      size_t max_compressed_size = LZ4F_compressFrameBound(uncompressed_size, &kPrefs);
      out_buffer = new char[max_compressed_size];
      wsize = LZ4F_compressFrame(out_buffer, max_compressed_size,
				 iter.second->data(), uncompressed_size, &kPrefs);
    }
    delete iter.second;
    iter.second = nullptr;
    fFragmentSize -= uncompressed_size;
    idx_to_clear.push_back(chunk_index);

    std::ofstream writefile(GetFilePath(chunk_index, true), std::ios::binary);
    writefile.write(out_buffer, wsize);
    delete[] out_buffer;
    writefile.close();

    // shenanigans or skulduggery?
    if(fs::exists(GetFilePath(chunk_index, false))) {
      fLog->Entry(MongoLog::Warning, "Chunk %s from thread %lx already exists? %li vs %li bytes",
          chunk_index.c_str(), fThreadId, fs::file_size(GetFilePath(chunk_index, false)), wsize);
    }

    // Move this chunk from *_TEMP to the same path without TEMP
    if(!fs::exists(GetDirectoryPath(chunk_index, false)))
      fs::create_directory(GetDirectoryPath(chunk_index, false));
    fs::rename(GetFilePath(chunk_index, true),
	       GetFilePath(chunk_index, false));
    comp_end = system_clock::now();
    fCompTime += duration_cast<microseconds>(comp_end-comp_start);

    CreateMissing(std::stoi(iter.first));
  } // End for through fragments
  // clear now because c++ sometimes overruns its buffers
  for (auto s : idx_to_clear) {
    if (fFragments.count(s) != 0) fFragments.erase(s);
  }

  if(end){
    for (auto& p : fFragments)
        if (p.second != nullptr) delete p.second;
    fFragments.clear();
    fFragmentSize = 0;
    fs::path write_path(fOutputPath);
    std::string filename = fHostname;
    write_path /= "THE_END";
    if(!fs::exists(write_path)){
      fLog->Entry(MongoLog::Local,"Creating END directory at %s",write_path.c_str());
      try{
        fs::create_directory(write_path);
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

std::string StraxOutput::GetStringFormat(int id){
  std::string chunk_index = std::to_string(id);
  while(chunk_index.size() < fChunkNameLength)
    chunk_index.insert(0, "0");
  return chunk_index;
}

fs::path StraxOutput::GetDirectoryPath(std::string id, bool temp){
  fs::path write_path(fOutputPath);
  write_path /= id;
  if(temp)
    write_path+="_temp";
  return write_path;
}

fs::path StraxOutput::GetFilePath(std::string id, bool temp){
  fs::path write_path = GetDirectoryPath(id, temp);
  std::string filename = fHostname;
  std::stringstream ss;
  ss<<std::this_thread::get_id();
  filename += "_";
  filename += ss.str();
  write_path /= filename;
  return write_path;
}

void StraxOutput::CreateMissing(u_int32_t back_from_id){

  for(unsigned int x=fMissingVerified; x<back_from_id; x++){
    std::string chunk_index = GetStringFormat(x);
    std::string chunk_index_pre = chunk_index+"_pre";
    std::string chunk_index_post = chunk_index+"_post";
    if(!fs::exists(GetFilePath(chunk_index, false))){
      if(!fs::exists(GetDirectoryPath(chunk_index, false)))
	fs::create_directory(GetDirectoryPath(chunk_index, false));
      std::ofstream o;
      o.open(GetFilePath(chunk_index, false));
      o.close();
    }
    if(x!=0 && !fs::exists(GetFilePath(chunk_index_pre, false))){
      if(!fs::exists(GetDirectoryPath(chunk_index_pre, false)))
	fs::create_directory(GetDirectoryPath(chunk_index_pre, false));
      std::ofstream o;
      o.open(GetFilePath(chunk_index_pre, false));
      o.close();
    }
    if(!fs::exists(GetFilePath(chunk_index_post, false))){
      if(!fs::exists(GetDirectoryPath(chunk_index_post, false)))
	fs::create_directory(GetDirectoryPath(chunk_index_post, false));
      std::ofstream o;
      o.open(GetFilePath(chunk_index_post, false));
      o.close();
    }
  }
  fMissingVerified = back_from_id;
}

