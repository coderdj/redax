#include "Compressor.hh"
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

Compressor::Compressor(std::shared_ptr<ThreadPool>& tp, std::shared_ptr<Processor>& next,
    std::shared_ptr<Options>& opts, std::share_ptr<MongoLog>& log) :
    Processor(tp, next, opts, log) {
  fNumWorkers = fOptions->GetNestedInt("output_files." + fHostname, 4);
  fWorkers.reserve(fNumWorkers);
  for (int i = 0; i < fNumWorkers; i++)
    fWorkers.emplace_back(opts, log, fHostname, i);
}

Compressor::~Compressor(){
  fWorkers.clear();
}

CompressorWorker::CompressorWorker(std::shared_ptr<Options>& opts, std::shared_ptr<MongoLog>& log, std::string hostname, int id) {
  fOptions = opts;
  fLog = log;
  fChunkNameLength=6;

  fChunkLength = long(fOptions->GetDouble("strax_chunk_length", 5)*1e9); // default 5s
  fChunkOverlap = long(fOptions->GetDouble("strax_chunk_overlap", 0.5)*1e9); // default 0.5s
  fCompressor = fOptions->GetString("compressor", "lz4");
  fFullChunkLength = fChunkLength+fChunkOverlap;
  std::string run_name = fOptions->GetString("run_identifier", "run");

  fEmptyVerified = 0;

  fBufferNumChunks = fOptions->GetInt("strax_buffer_num_chunks", 2);
  fWarnIfChunkOlderThan = fOptions->GetInt("strax_chunk_phase_limit", 1);

  std::string output_path = fOptions->GetString("strax_output_path", "./");
  try{
    fs::path op(output_path);
    op /= run_name;
    fOutputPath = op;
    fs::create_directory(op);
  }catch(...){
    fLog->Entry(MongoLog::Error, "Compressor::Initialize tried to create output directory but failed. Check that you have permission to write here.");
    throw std::exception("Could not create output directory");
  }
  fMinChunk = fMaxChunk = 0;
}

CompressorWorker::~CompressorWorker() {
  fs::path write_path(fOutputPath);
  write_path /= "THE_END";
  if(!fs::exists(write_path)){
    fLog->Entry(MongoLog::Local,"Creating END directory at %s",write_path.c_str());
    try{
      fs::create_directory(write_path);
    }
    catch(...){};
  }
  auto outpath = write_path / fHostname + "_" + std::to_string(fID);
  std::ofstream outfile(outpath, std::ios::out);
  outfile<<"...my only friend";
  outfile.close();
}

std::u32string CompressorWorker::AddFragmentToBuffer(std::string&& fragment) {
  int64_t timestamp = *(int64_t*)fragment.data();
  // Get the CHUNK and decide if this event also goes into a PRE/POST file
  int chunk_id = timestamp/fFullChunkLength;
  bool overlap = (chunk_id+1)* fFullChunkLength - timestamp <= fChunkOverlap;
  // Minor mess to maintain the same width of file names and do the pre/post stuff
  {
    const std::lock_guard<std::mutex> lg(fMutex);
    if (fMinChunk - chunk_id > fWarnIfChunkOlderThan) {
      int16_t ch = *(int16_t*)(fragment.data()+14);
      fLog->Entry(MongoLog::Warning, "Thread %i received a fragment from CH%i %i chunks behind phase (%i/%i)?",
          fID, ch, fMinChunk-chunk_id, fMinChunk, fMaxChunk);
    }

    if(!overlap){
      fBuffer[chunk_id].emplace_back(std::move(fragment));
    } else {
      fOverlapBuffer[chunk_id].emplace_back(std::move(fragment));
    }
    auto [_min, _max] = std::minmax_element(fBuffer.begin(), fBuffer.end(),
        [&](auto& a, auto& b){return a.first < b.first;});
    fMinChunk = (*_min).first;
    fMaxChunk = (*_max).first;

    if (fMaxChunk - fBufferNumChunks > fMinChunk) {
      int write_lte = fMaxChunk - fBufferNumChunks;
      std::u32string task;
      task.reserve(fBuffers.size()+fBufferNumChunks);
      task += ThreadPool::TaskCode::CompressChunk;
      char32_t word = fID;
      task += word;
      for (auto& p : fBuffer) if (p.first <= write_lte) task += p.first;
      return task;
    }
  }
  return "";
}

void Compressor::End() {
  for (auto& w : fWorkers) fTP->AddTask(this, std::move(w.End()));
}

std::u32string CompressorWorker::End() {
  // returns a Task for all remaining data
  const std::lock_guard<std::mutex> lg(fMutex);
  std::u32string ret;
  ret.reserve(fBuffer.size()+2);
  ret += ThreadPool::TaskCode::CompressChunk;
  ret += fID;
  for (auto& p : fBuffer) ret += p.first;
  return ret;
}

void Compressor::Process(std::string_view input) {
  int buffer_num = input[1];
  input.remove_prefix(2);
  for (int chunk : input) {
    fWorkers[buffer_num].WriteOutChunk(chunk);
  }
}

// Can tune here as needed, these are defaults from the LZ4 examples
static const LZ4F_preferences_t kPrefs = {
  { LZ4F_max256KB, LZ4F_blockLinked, LZ4F_noContentChecksum, LZ4F_frame, 0, { 0, 0 } },
    0,   /* compression level; 0 == default */
    0,   /* autoflush */
    { 0, 0, 0 },  /* reserved, must be set to 0 */
};

void CompressorWorker::WriteOutChunk(int chunk_i){
  // Write the contents of fFragments to compressed files
  auto buffers = {&fBuffer[chunk_i], &fOverlapBuffer[chunk_i]};
  std::vector<size_t> uncompressed_size(2);
  std::vector<std::string> uncompressed(2);
  {
    const std::lock_guard<std::mutex> lg(fMutex);

    for (int i = 0; i < 2; i++) {
      uncompressed_size[i] = std::accumulate(buffers[i]->begin(),
          buffers[i]->end(), 0L,
          [&](auto tot, auto& s){return std::move(tot) + s.size();});
      uncompressed[i].reserve(uncompresed_size[i]);
      for (auto it = buffers[i]->begin(); it < buffers[i]->end(); it++)
        uncompressed[i] += *it;
      buffers[i]->clear();
    }
    fBuffer[buffer_num].erase(chunk_i);
    fOverlapBuffer[buffer_num].erase(chunk_i);

    auto [_min, _max] = std::minmax_element(fBuffer.begin(), fBuffer.end(),
        [&](auto& a, auto& b){return a.first < b.first;});
    fMinChunk = (*_min).first;
    fMaxChunk = (*_max).first;
  }

  // Compress it
  std::vector<unique_ptr<char[]>>(2);
  std::vector<int> wsize(2, 0);
  for (int i = 0; i < 2; i++) {
    if(fCompressor == "blosc"){
      out_buffer[i] = std::make_unique<char[]>(uncompressed_size+BLOSC_MAX_OVERHEAD);
      wsize[i] = blosc_compress_ctx(5, 1, sizeof(char), uncompressed_size[i],
          uncompressed[i].data(), out_buffer[i].get(), 
          uncompressed_size[i]+BLOSC_MAX_OVERHEAD, "lz4", 0, 2);
    }else{
      // Note: the current package repo version for Ubuntu 18.04 (Oct 2019) is 1.7.1,
      // which is so old it is not tracked on the lz4 github. The API for frame
      // compression has changed just slightly in the meantime. So if you update and
      // it breaks you'll have to tune at least the LZ4F_preferences_t object to
      // the new format.
      size_t max_compressed_size = LZ4F_compressFrameBound(uncompressed_size[i], &kPrefs);
      out_buffer[i] = std::make_unique<char[]>(max_compressed_size);
      wsize[i] = LZ4F_compressFrame(out_buffer[i].get(), max_compressed_size,
          uncompressed[i].data(), uncompressed_size[i], &kPrefs);
  }

  // write to *_TEMP
  std::string chunk_index = GetStringFormat(chunk_i);
  auto output_dir_temp = GetDirectoryPath(chunk_index, true);
  auto output_dir = GetDirectoryPath(chunk_index, false);
  if(!fs::exists(output_dir_temp))
    fs::create_directory(output_dir_temp);
  auto filename_temp = GetFilePath(chunk_index, true);
  auto filename = GetFilePath(chunk_index, false);
  std::ofstream writefile(filename_temp, std::ios::binary);
  writefile.write(out_buffer[0].get(), wsize[0]);
  out_buffer[0].reset();
  writefile.close();

  // shenanigans or skulduggery?
  if(fs::exists(filename)) {
    fLog->Entry(MongoLog::Warning, "Chunk %s from thread %x already exists? %li vs %li bytes",
          chunk_index.c_str(), buffer_num, fs::file_size(filename), wsize[0]);
  }

  // Move this chunk from *_TEMP to the same path without TEMP
  if(!fs::exists(output_dir))
    fs::create_directory(outputdir);
  fs::rename(filename_temp, filename);

  // now redo half again for the overlap chunk
  auto chunk_names = {GetStringFormat(chunk_i) + "_post",
    GetStringFormat(chunk_i+1) + "_pre"};
  for (auto& n : chunk_names) {
    output_dir_temp = GetDirectoryPath(n, true);
    output_dir = GetDirectoryPath(n, false);
    filename_temp = GetFilePath(n, true);
    filename = GetFilePath(n, false);
    if (!fs::exists(output_dir_temp))
      fs::create_directory(output_dir_temp);
    std::ofstream fout(filename_temp, std::ios::binary);
    fout.write(out_buffer[1].get(), wsize[1]);
    fout.close();
    // check for overwrite
    if (fs::exists(filename)) {
      fLog->Entry(MongoLog::Warning, "Chunk %s from thread %x already exists? %li vs %li bytes",
          n.c_str(), buffer_num, fs::file_size(filename), wsize[1]);
    }
    // rename from *_TEMP to not _TEMP
    if (!fs::exists(output_dir))
      fs::create_directory(ouput_dir);
    fs::rename(filename_temp, filename);
  }
  out_buffer[1].reset();

  CreateEmpty(chunk_i);
}

void Compressor::End() {

}

std::string CompressorWorker::GetStringFormat(int id){
  std::string chunk_index = std::to_string(id);
  while(chunk_index.size() < fChunkNameLength)
    chunk_index.insert(0, "0");
  return chunk_index;
}

fs::path CompressorWorker::GetDirectoryPath(const std::string& id, bool temp){
  fs::path write_path(fOutputPath);
  write_path /= id;
  if(temp) write_path+="_temp";
  return write_path;
}

fs::path CompressorWorker::GetFilePath(const std::string& id, bool temp){
  fs::path write_path = GetDirectoryPath(id, temp);
  std::string filename = fHostname;
  filename += "_" + std::to_string(fID);
  write_path /= filename;
  return write_path;
}

void CompressorWorker::CreateEmpty(int check_up_to){
  int chunk = fEmptyVerified;
  fEmptyVerified = check_up_to;
  for(; chunk<check_up_to; chunk++){
    std::string chunk_index = GetStringFormat(chunk);
    std::string chunk_index_pre = chunk_index+"_pre";
    std::string chunk_index_post = chunk_index+"_post";
    if(!fs::exists(GetFilePath(chunk_index, false))){
      if(!fs::exists(GetDirectoryPath(chunk_index, false)))
	fs::create_directory(GetDirectoryPath(chunk_index, false));
      std::ofstream o(GetFilePath(chunk_index, false));
      o.close();
    }
    if(chunk!=0 && !fs::exists(GetFilePath(chunk_index_pre, false))){
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
}

