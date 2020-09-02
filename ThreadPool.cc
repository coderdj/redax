#include "ThreadPool.hh"
#include <functional>


ThreadPool::ThreadPool(int num_threads) {
  fFinishNow = false;
  fWaitingTasks = fRunningTasks = 0;
  fThreads.reserve(num_threads);
  for (; num_threads > 0; num_threads--) fThreads.emplace_back(std::thread(&ThreadPool::Run, this));
}

ThreadPool::~ThreadPool() {
  Kill();
  fWaitingTasks = 0;
  for (auto& t : fThreads) t.join();
  fThreads.clear();
  fQueue.clear();
}

void AddTask(Processor* obj, std::string&& input) {
  {
    std::lock_guard<std::mutex> lg(fMutex);
    fQueue.emplace_back(new task_t{obj, std::move(input)});
    fWaitingTasks++;
  }
  fCV.notify_one();
}

void ThreadPool::Run() {
  std::unique_ptr<task_t> task;
  while (!fFinishNow) {
    std::unique_lock<std::mutex> lk(fMutex);
    fCV.wait(lk, [&]{return fWaitingTasks > 0 || fFinishNow;});
    if (fQueue.size() > 0 && !fFinishNow) {
      task = std::move(fQueue.front());
      fQueue.pop_font();
      fWaitingTasks--;
      fRunningTasks++;
      lk.unlock();
      std::invoke(&Processor::Process, task->obj, task->input);
      task.reset();
      fRunningTasks--;
    } else {
      lk.unlock();
    }
  }
}
