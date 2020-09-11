#ifndef _COMPRESSOR_HH_
#define _COMPRESSOR_HH_

#include <string>
#include <map>
#include <mutex>
#include <experimental/filesystem>
#include <atomic>
#include <vector>
#include "Processor.hh"

class Compressor : public Processor{
  /*
    Writes chunks to disk
  */

public:
  Compressor(std::shared_ptr<ThreadPool>&, std::shared_ptr<Processor>&, std::shared_ptr<Options>&, std::shared_ptr<MongoLog>&);
  ~Compressor();

  void Process(std::u32string_view);
  void AddFragmentToBuffer(std::string&&);
  void End();

private:
  int SelectBuffer();
  int fNumWorkers;
  std::vector<CompressorWorker> fWorkers;
  std::atomic_long fSelector; // int probably fine? Meh.
};

inline int Compressor::SelectBuffer() {
  return (++fSelector) % fNumWorkers;
}

inline void Compressor::AddFragmentToBuffer(std::string&& frag) {
  auto s = fWorkers[SelectBuffer()].AddFragmentToBuffer(std::move(frag));
  if (s) fTP->AddTask(this, std::move(s));
  return;
}

class CompressorWorker {
public:
  CompressorWorker(std::shared_ptr<Options>&, std::shared_ptr<MongoLog>&, std::string, std::string, int);
  ~CompressorWorker();

  void WriteOutChunk(int);
  std::u32string AddFragmentToBuffer(std::string&&);
  std::u32string End();

private:
  std::experimental::filesystem::path GetFilePath(const std::string&, bool);
  std::experimental::filesystem::path GetDirectoryPath(const std::string&, bool);
  std::string GetStringFormat(int);
  void CreateEmpty(uint32_t);

  std::map<int, std::list<std::string>> fBuffer;
  std::map<int, std::list<std::string>> fOverlapBuffer;
  std::mutex fMutex;
  int fMinChunk, fMaxChunk;
  std::atomic_int fEmptyVerified;
  std::string fOutputPath, fHostname;
  std::string fCompressor;

  int fBufferNumChunks;
  int fWarnIfChunkOlderThan;
  int fNumBuffers;
  unsigned fChunkNameLength;
  int64_t fFullChunkLength;
  int fID;

  std::atomic_int fEmptyVerified;

};

#endif // _COMPRESSOR_HH_ defined
