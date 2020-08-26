#ifndef _THREAD_POOL_HH_
#define _THREAD_POOL_HH_

#include <list>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <string>
#include <memory>

// ABC to support thread pool
class Processor {
  public:
    Processor() {}
    virtual ~Processor();
    virtual void Process(std::string&)=0;
};

typedef void (Processor::*WorkFunction)(std::string& input);

class ThreadPool {
  public:
    ThreadPool(int);
    ~ThreadPool();

    void AddTask(WorkFunction, Processor*, std::string&&);
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
      WorkFunction func;
      Processor* obj;
      std::string input;
    };
#endif // _THREAD_POOL_HH_ defined
