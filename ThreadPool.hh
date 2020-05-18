#ifndef _THREAD_POOL_HH_
#define _THREAD_POOL_HH_

#include <list>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <function>

class ThreadPool {
  public:
    ThreadPool(int);
    ~ThreadPool();

    void AddTask(std::function<void>&&);
    void Kill() {fFinishNow = true; fCV.notify_all()}
    int GetWaiting() {return fWaitingTasks.load();}
    int GetRunning() {return fRunningTasks.load();}

  private:
    void Run();
    std::vector<std::thread> fThreads;
    std::list<std::function<void()>> fQueue;
    std::mutex fMutex;
    std::condition_variable fCV;

    std::atomic_bool fFinishNow;
    std::atomic_int fWaitingTasks, fRunningTasks;
};

#endif // _THREAD_POOL_HH_ defined
