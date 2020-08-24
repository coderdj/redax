#ifndef _THREAD_POOL_HH_
#define _THREAD_POOL_HH_

#include <list>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

class ThreadPool {
  public:
    ThreadPool(int);
    ~ThreadPool();

    void AddTask((void*)(std::string&), std::string&&);
    void Kill() {fFinishNow = true; fCV.notify_all();}
    int GetWaiting() {return fWaitingTasks.load();}
    int GetRunning() {return fRunningTasks.load();}

  private:
    void Run();
    std::vector<std::thread> fThreads;
    std::list<task> fQueue;
    std::mutex fMutex;
    std::condition_variable fCV;

    std::atomic_bool fFinishNow;
    std::atomic_int fWaitingTasks, fRunningTasks;

    struct task_t {
      void (*func)(std::string&);
      std::string input;
    };
};

#endif // _THREAD_POOL_HH_ defined
