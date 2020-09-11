#include "ThreadPool.hh"
#include "Processor.hh"
#include <functional>
#include <ctime>

inline double timespec_subtract(const struct timespec& a, const struct timespec& b) {
  return (a.tv_sec - b.tv_sec)*1e6 + (a.tv_nsec - b.tv_nsec)*0.001;
}

ThreadPool::ThreadPool(int num_threads) {
  fFinishNow = false;
  fWaitingTasks = fRunningTasks = 0;
  fBufferBytes = 0;
  fThreads.reserve(num_threads);
  for (int i = 0; i < num_threads; i++) fThreads.emplace_back(std::thread(&ThreadPool::Run, this));
}

ThreadPool::~ThreadPool() {
  fWaitingTasks = 0;
  Kill();
  for (auto& t : fThreads) t.join();
  fThreads.clear();
  fQueue.clear();
}

void AddTask(Processor* obj, std::u32string&& input) {
  {
    std::lock_guard<std::mutex> lg(fMutex);
    fBufferBytes += input.size()*sizeof(char32_t);
    fQueue.emplace_back(new task_t{obj, std::move(input)});
    fWaitingTasks++;
  }
  fCV.notify_one();
}

void ThreadPool::Run() {
  std::unique_ptr<task_t> task;
  struct timespec start, stop;
  std::map<TaskCode, double> benchmarks;
  TaskCode code;
  while (!fFinishNow) {
    std::unique_lock<std::mutex> lk(fMutex);
    fCV.wait(lk, [&]{return fWaitingTasks > 0 || fFinishNow;});
    if (fQueue.size() > 0 && !fFinishNow) {
      task = std::move(fQueue.front());
      fQueue.pop_font();
      fWaitingTasks--;
      fRunningTasks++;
      lk.unlock();
      fBufferBytes -= task->input.size()*sizeof(char32_t);
      code = task->input[0];
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &start);
      std::invoke(&Processor::Process, task->obj, task->input);
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end);
      task.reset();
      fRunningTasks--;
      benchmarks[code] += timespec_subtract(end, start);
    } else {
      lk.unlock();
    }
  }
  {
    const std::unique_lock<std::mutex> lk(fMutex_);
    for (auto& p : benchmarks) fBenchmarks[p.first] += p.second;
  }
}
