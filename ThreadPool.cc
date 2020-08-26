#include "ThreadPool.hh"
#include <functional>


ThreadPool::ThreadPool(int num_threads) {
  fFinishNow = false;
  fWaitingTasks = fRunningTasks = 0;
  fThreads.reserve(num_threads);
  for (int i = 0; i < num_threads; i++) fThreads.emplace_back(std::thread(&ThreadPool::Run, this));
}

ThreadPool::~ThreadPool() {
  Kill();
  std::unique_lock<std::mutex> lk(fMutex);
  fQueue.clear();
  fWaitingTasks = 0;
  for (auto& t : fThreads) t.join();
  fThreads.clear();
}

void AddTask(WorkFunction func, Processor* obj, std::string&& input) {
  task_t* t = new Task();
  t->func = func;
  t->obj = obj;
  t->input = input;
  {
    std::lock_guard<std::mutex> lg(fMutex);
    fQueue.emplace_back(t);
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
      task = fQueue.front();
      fQueue.pop_font();
      fWaitingTasks--;
      fRunningTasks++;
      lk.unlock();
      std::invoke(task->func, *(task->obj), task->input);
      task.reset();
      fRunningTasks--;
    } else {
      lk.unlock();
    }
  }
}
