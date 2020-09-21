#ifndef _THREAD_POOL_HH_
#define _THREAD_POOL_HH_

#include <list>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <string>
#include <memory>
#include <map>
#include <condition_variable>

class Processor;

class ThreadPool {
  public:
    ThreadPool(int, int);
    ~ThreadPool();

    void AddTask(Processor*, std::u32string);
    void AddTask(Processor*, std::vector<std::u32string>&);
    int GetWaiting() {return fWaitingTasks.load();}
    int GetRunning() {return fRunningTasks.load();}
    long GetBytes() {return fBufferBytes.load();}
    void PrintStatus();

    enum TaskCode : char32_t{
      UnpackDatapacket = 0,
      UnpackChannel,
      CompressChunk,
      // WFsim stuff
      GenerateWaveform,

      NUM_CODES
    };

  private:
    void Run();
    void Kill() {fFinishNow = true; fCV.notify_all();}

    struct task_t {
      Processor* obj;
      std::u32string input;
      TaskCode code() {return input.size() > 0 ? static_cast<TaskCode>(input[0]) : TaskCode::NUM_CODES;}
    };

    int fBytesPerPull;

    std::vector<std::thread> fThreads;
    std::list<std::unique_ptr<task_t>> fQueue;
    std::condition_variable fCV;

    std::atomic_bool fFinishNow;
    std::atomic_int fWaitingTasks, fRunningTasks;
    std::atomic_long fBufferBytes;

    std::map<TaskCode, double> fBenchmarks;
    std::map<TaskCode, long> fCounter;
    std::mutex fMutex, fMutex_;
};

#endif // _THREAD_POOL_HH_ defined
