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
    std::shared_ptr<Options>& opts, std::shared_ptr<MongoLog>& log) :
    Processor(tp, next, opts, log) {
  fNumWorkers = fOptions->GetNestedInt("output_files." + fOptions->fHostname, 4);
  std::string host = fOptions->fHostname;
  fWorkers.reserve(fNumWorkers);
  for (int i = 0; i < fNumWorkers; i++)
    fWorkers.emplace_back(std::make_unique<CompressorWorker>(opts, log, host, i));
}

Compressor::~Compressor(){
  fWorkers.clear();
}

Compressor::CompressorWorker::CompressorWorker(std::shared_ptr<Options>& opts, std::shared_ptr<MongoLog>& log, std::string hostname, int id) {
  fLog = log;
  fID = id;
  fHostname = hostname;
  fChunkNameLength=6;

  fChunkLength = long(opts->GetDouble("strax_chunk_length", 5)*1e9); // default 5s
  fChunkOverlap = long(opts->GetDouble("strax_chunk_overlap", 0.5)*1e9); // default 0.5s
  int header_size=24;
  fBytesPerFrag = opts->GetInt("strax_fragment_payload_bytes", 220) + header_size;
  fCompressor = opts->GetString("compressor", "lz4");
  fFullChunkLength = fChunkLength+fChunkOverlap;
  std::string run_name = opts->GetString("run_identifier", "run");

  fEmptyVerified = 0;

  fBufferNumChunks = opts->GetInt("strax_buffer_num_chunks", 2);
  fWarnIfChunkOlderThan = opts->GetInt("strax_chunk_phase_limit", 1);

  std::string output_path = opts->GetString("strax_output_path", "./");
  try{
    fs::path op(output_path);
    op /= run_name;
    fOutputPath = op;
    fs::create_directory(op);
  }catch(...){
    fLog->Entry(MongoLog::Error, "Compressor::Initialize tried to create output directory but failed. Check that you have permission to write here.");
    throw std::runtime_error("Could not create output directory");
  }
  fMinChunk = fMaxChunk = 0;
}

Compressor::CompressorWorker::~CompressorWorker() {
  fs::path write_path(fOutputPath);
  write_path /= "THE_END";
  if(!fs::exists(write_path)){
    fLog->Entry(MongoLog::Local,"Creating END directory at %s",write_path.c_str());
    try{
      fs::create_directory(write_path);
    }
    catch(...){};
  }
  std::string filename = fHostname + "_" + std::to_string(fID);
  auto outpath = write_path / filename;
  std::ofstream outfile(outpath, std::ios::out);
  outfile<<"...my only friend";
  outfile.close();
}

void Compressor::AddFragmentToBuffer(std::string frag) {
  auto s = fWorkers[SelectBuffer()]->AddFragmentToBuffer(std::move(frag));
  if (s.size()>0) fTP->AddTask(this, std::move(s));
  return;
}

void Compressor::AddFragmentToBuffer(std::vector<std::string>& frags) {
  auto s = fWorkers[SelectBuffer()]->AddFragmentToBuffer(frags);
  if (s.size()>0) fTP->AddTask(this, std::move(s));
  return;
}

std::u32string Compressor::CompressorWorker::AddFragmentToBuffer(std::string fragment) {
  int64_t timestamp = *(int64_t*)fragment.data();
  // Get the CHUNK and decide if this event also goes into a PRE/POST file
  int chunk_id = timestamp/fFullChunkLength;
  bool overlap = (chunk_id+1)* fFullChunkLength - timestamp <= fChunkOverlap;
  {
    const std::lock_guard<std::mutex> lg(fMutex);
    if (fMinChunk - chunk_id > fWarnIfChunkOlderThan) {
      int16_t ch = *(int16_t*)(fragment.data()+14);
      fLog->Entry(MongoLog::Warning, "Thread %i received a fragment from CH%i %i chunks behind phase (%i/%i)?",
          fID, ch, fMinChunk-chunk_id, fMinChunk.load(), fMaxChunk.load());
    } else if (chunk_id - fMaxChunk > 1) {
      fLog->Entry(MongoLog::Message, "Thread %i skipped %i chunks (%i/%i/%i)",
              fID, chunk_id-fMaxChunk-1, fMinChunk.load(), fMaxChunk.load(), chunk_id);
    }

    if(!overlap){
      fBuffer[chunk_id].emplace_back(std::move(fragment));
    } else {
      fOverlapBuffer[chunk_id].emplace_back(std::move(fragment));
    }
    for (auto& p : fBuffer)
      if (p.second.size() > 10) // protects against one random fragment
        fMaxChunk = std::max(p.first, fMaxChunk.load());

    if (fMaxChunk - fBufferNumChunks >= fMinChunk) {
      int write_lte = fMaxChunk - fBufferNumChunks;
      fLog->Entry(MongoLog::Local, "CW %i %i/%i write %i",
              fID, fMinChunk.load(), fMaxChunk.load(), write_lte);
      fMinChunk = write_lte + 1;
      std::u32string task;
      task.reserve(fBuffer.size()+fBufferNumChunks);
      task += ThreadPool::TaskCode::CompressChunk;
      char32_t word = fID;
      task += word;
      for (auto& p : fBuffer) if (p.first <= write_lte) task += p.first;
      return task;
    } else {
    }
  }
  return std::u32string();
}

std::u32string Compressor::CompressorWorker::AddFragmentToBuffer(std::vector<std::string>& fragments) {
  bool warn = true;
  {
    const std::lock_guard<std::mutex> lg(fMutex);
    for (auto& fragment : fragments) {
      int64_t timestamp = *(int64_t*)fragment.data();
      // Get the CHUNK and decide if this event also goes into a PRE/POST file
      int chunk_id = timestamp/fFullChunkLength;
      bool overlap = (chunk_id+1)* fFullChunkLength - timestamp <= fChunkOverlap;
      if (fMinChunk - chunk_id > fWarnIfChunkOlderThan && warn) {
        int16_t ch = *(int16_t*)(fragment.data()+14);
        fLog->Entry(MongoLog::Warning, "Thread %i received a fragment from CH%i %i chunks behind phase (%i/%i)?",
          fID, ch, fMinChunk-chunk_id, fMinChunk.load(), fMaxChunk.load());
        warn = false;
      } else if (chunk_id - fMaxChunk > 1 && warn) {
        fLog->Entry(MongoLog::Message, "Thread %i skipped %i chunks (%i/%i/%i)",
              fID, chunk_id-fMaxChunk-1, fMinChunk.load(), fMaxChunk.load(), chunk_id);
        warn = false;
      }

      if(!overlap){
        fBuffer[chunk_id].emplace_back(std::move(fragment));
      } else {
        fOverlapBuffer[chunk_id].emplace_back(std::move(fragment));
      }
    } // frag in fragments
    for (auto& p : fBuffer)
      if (p.second.size() > 10) // protects against one random fragment
        fMaxChunk = std::max(p.first, fMaxChunk.load());

    if (fMaxChunk - fBufferNumChunks >= fMinChunk) {
      int write_lte = fMaxChunk - fBufferNumChunks;
      fLog->Entry(MongoLog::Local, "CW %i %i/%i write %i",
              fID, fMinChunk.load(), fMaxChunk.load(), write_lte);
      fMinChunk = write_lte + 1;
      std::u32string task;
      task.reserve(fBuffer.size()+fBufferNumChunks);
      task += ThreadPool::TaskCode::CompressChunk;
      char32_t word = fID;
      task += word;
      for (auto& p : fBuffer) if (p.first <= write_lte) task += p.first;
      return task;
    } else {
    }
  }
  return std::u32string();
}

void Compressor::End() {
  for (auto& w : fWorkers) fTP->AddTask(this, std::move(w->End()));
}

std::u32string Compressor::CompressorWorker::End() {
  // returns a Task for all remaining data
  const std::lock_guard<std::mutex> lg(fMutex);
  std::u32string ret;
  ret.reserve(fBuffer.size()+2);
  ret += ThreadPool::TaskCode::CompressChunk;
  ret += fID;
  for (auto& p : fBuffer) ret += p.first;
  return ret;
}

void Compressor::Process(std::u32string_view input) {
  int buffer_num = input[1];
  input.remove_prefix(2);
  for (int chunk : input) {
    fWorkers[buffer_num]->WriteOutChunk(chunk);
  }
}

// Can tune here as needed, these are defaults from the LZ4 examples
static const LZ4F_preferences_t kPrefs = {
  { LZ4F_max256KB, LZ4F_blockLinked, LZ4F_noContentChecksum, LZ4F_frame, 0, { 0, 0 } },
    0,   /* compression level; 0 == default */
    0,   /* autoflush */
    { 0, 0, 0 },  /* reserved, must be set to 0 */
};

void Compressor::CompressorWorker::WriteOutChunk(int chunk_i){
  // Write the contents of fFragments to compressed files
  std::vector<std::list<std::string>*> buffers = {&fBuffer[chunk_i], &fOverlapBuffer[chunk_i]};
  std::vector<size_t> uncompressed_size(3, 0);
  std::vector<std::string> uncompressed(2);
  {
    const std::lock_guard<std::mutex> lg(fMutex);

    for (int i = 0; i < 2; i++) {
      uncompressed_size[i] = buffers[i]->size()*fBytesPerFrag;
      uncompressed[i].reserve(uncompressed_size[i]);
      for (auto it = buffers[i]->begin(); it != buffers[i]->end(); it++)
        uncompressed[i] += *it;
      buffers[i]->clear();
    }
    //fLog->Entry(MongoLog::Local, "CW %i chunk %i size %i %i", fID, chunk_i,
    //        uncompressed_size[0], uncompressed_size[1]);
    fBuffer.erase(chunk_i);
    fOverlapBuffer.erase(chunk_i);

    auto [_min, _max] = std::minmax_element(fBuffer.begin(), fBuffer.end(),
        [&](auto& a, auto& b){return a.first < b.first;});
    fMinChunk = (*_min).first;
    fMaxChunk = (*_max).first;
  }

  // Compress it
  std::vector<std::shared_ptr<std::string>> out_buffer(3);
  std::vector<int> wsize(3, 0);
  for (int i = 0; i < 2; i++) {
    if (uncompressed_size[0] == 0) continue;
    size_t max_compressed_size = 0;
    if(fCompressor == "blosc"){
      max_compressed_size = uncompressed_size[i]+BLOSC_MAX_OVERHEAD;
      out_buffer[i] = std::make_shared<std::string>(max_compressed_size, 0);
      wsize[i] = blosc_compress_ctx(5, 1, sizeof(char), uncompressed_size[i],
          uncompressed[i].data(), out_buffer[i]->data(), 
          max_compressed_size, "lz4", 0, 2);
    }else{
      // Note: the current package repo version for Ubuntu 18.04 (Oct 2019) is 1.7.1,
      // which is so old it is not tracked on the lz4 github. The API for frame
      // compression has changed just slightly in the meantime. So if you update and
      // it breaks you'll have to tune at least the LZ4F_preferences_t object to
      // the new format.
      max_compressed_size = LZ4F_compressFrameBound(uncompressed_size[i], &kPrefs);
      out_buffer[i] = std::make_shared<std::string>(max_compressed_size, 0);
      wsize[i] = LZ4F_compressFrame(out_buffer[i]->data(), max_compressed_size,
          uncompressed[i].data(), uncompressed_size[i], &kPrefs);
    }
  }
  uncompressed_size[2] = uncompressed_size[1];
  out_buffer[2] = out_buffer[1];
  wsize[2] = wsize[1];

  std::vector<std::string> names = {GetStringFormat(chunk_i), GetStringFormat(chunk_i)+"_post", GetStringFormat(chunk_i+1)+"_pre"};
  for (int i = 0; i < 3; i++) {
    // write to *_TEMP
    auto output_dir_temp = GetDirectoryPath(names[i], true);
    auto output_dir = GetDirectoryPath(names[i], false);
    if(!fs::exists(output_dir_temp))
      fs::create_directory(output_dir_temp);
    auto filename_temp = GetFilePath(names[i], true);
    auto filename = GetFilePath(names[i], false);
    std::ofstream writefile(filename_temp, std::ios::binary);
    if (uncompressed_size[i] > 0) writefile.write(out_buffer[i]->data(), wsize[i]);
    out_buffer[i].reset();
    writefile.close();
    fLog->Entry(MongoLog::Local, "Writing %s", filename.c_str());

    // shenanigans or skulduggery?
    if(fs::exists(filename)) {
      fLog->Entry(MongoLog::Warning, "Chunk %s from thread %x already exists? %li vs %li bytes",
          names[i].c_str(), fID, fs::file_size(filename), wsize[i]);
    }

    // Move this chunk from *_TEMP to the same path without TEMP
    if(!fs::exists(output_dir))
      fs::create_directory(output_dir);
    fs::rename(filename_temp, filename);
  }

  CreateEmpty(chunk_i);
  return;
}

std::string Compressor::CompressorWorker::GetStringFormat(int id){
  std::string chunk_index = std::to_string(id);
  while(chunk_index.size() < fChunkNameLength)
    chunk_index.insert(0, "0");
  return chunk_index;
}

fs::path Compressor::CompressorWorker::GetDirectoryPath(const std::string& id, bool temp){
  fs::path write_path(fOutputPath);
  write_path /= id;
  if(temp) write_path+="_temp";
  return write_path;
}

fs::path Compressor::CompressorWorker::GetFilePath(const std::string& id, bool temp){
  fs::path write_path = GetDirectoryPath(id, temp);
  std::string filename = fHostname;
  filename += "_" + std::to_string(fID);
  write_path /= filename;
  return write_path;
}

void Compressor::CompressorWorker::CreateEmpty(int check_up_to){
  int chunk = fEmptyVerified;
  fEmptyVerified = check_up_to;
  std::ofstream fout;
  for(; chunk<check_up_to; chunk++){
    std::string chunk_index = GetStringFormat(chunk);
    std::string chunk_index_pre = chunk_index+"_pre";
    std::string chunk_index_post = chunk_index+"_post";
    if(!fs::exists(GetFilePath(chunk_index, false))){
      if(!fs::exists(GetDirectoryPath(chunk_index, false)))
	fs::create_directory(GetDirectoryPath(chunk_index, false));
      fout.open(GetFilePath(chunk_index, false));
      fout.close();
    }
    if(chunk!=0 && !fs::exists(GetFilePath(chunk_index_pre, false))){
      if(!fs::exists(GetDirectoryPath(chunk_index_pre, false)))
	fs::create_directory(GetDirectoryPath(chunk_index_pre, false));
      fout.open(GetFilePath(chunk_index_pre, false));
      fout.close();
    }
    if(!fs::exists(GetFilePath(chunk_index_post, false))){
      if(!fs::exists(GetDirectoryPath(chunk_index_post, false)))
	fs::create_directory(GetDirectoryPath(chunk_index_post, false));
      fout.open(GetFilePath(chunk_index_post, false));
      fout.close();
    }
  }
}

