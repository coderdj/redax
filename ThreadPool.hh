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

class Processor;

class ThreadPool {
  public:
    ThreadPool(int);
    ~ThreadPool();

    void AddTask(Processor*, std::u32string&&);
    int GetWaiting() {return fWaitingTasks.load();}
    int GetRunning() {return fRunningTasks.load();}
    long GetBytes() {return fBufferBytes.load();}

    enum TaskCode : char32_t{
      UnpackDatapacket = 0,
      UnpackEvent,
      UnpackChannel,
      CompressChunk
    };

  private:
    void Run();
    void Kill() {fFinishNow = true; fCV.notify_all();}
    std::vector<std::thread> fThreads;
    std::list<std::unique_ptr<task_t>> fQueue;
    std::condition_variable fCV;

    std::atomic_bool fFinishNow;
    std::atomic_int fWaitingTasks, fRunningTasks;
    std::atomic_long fBufferBytes;

    struct task_t {
      Processor* obj;
      std::u32string input;
    };

    std::map<TaskCode, double> fBenchmarks;
    std::mutex fMutex, fMutex_;
};

#endif // _THREAD_POOL_HH_ defined
