#ifndef _THREAD_POOL_HH_
#define _THREAD_POOL_HH_

#include <list>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <string>
#include <string_view>
#include <memory>

class ThreadPool;

// ABC to support thread pool
class Processor {
  public:
    Processor(ThreadPool* tp, Processor* next) : fTP(tp), fNext(next) {}
    virtual ~Processor();
    virtual void Process(std::string_view)=0;

  protected:
    ThreadPool* fTP;
    Processor* fNext;
};

class ThreadPool {
  public:
    ThreadPool(int);
    ~ThreadPool();

    void AddTask(Processor*, std::string&&);
    void Kill() {fFinishNow = true; fCV.notify_all();}
    int GetWaiting() {return fWaitingTasks.load();}
    int GetRunning() {return fRunningTasks.load();}

  private:
    void Run();
    std::vector<std::thread> fThreads;
    std::list<std::unique_ptr<task_t>> fQueue;
    std::condition_variable fCV;

    std::atomic_bool fFinishNow;
    std::atomic_int fWaitingTasks, fRunningTasks;

    struct task_t {
      Processor* obj;
      std::string input;
    };
};

#endif // _THREAD_POOL_HH_ defined
