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
  for (int i = 0; i < num_threads; i++) fThreads.emplace_back(&ThreadPool::Run, this);
  fMaxPerPull = 16;
}

ThreadPool::~ThreadPool() {
  fWaitingTasks = 0;
  Kill();
  for (auto& t : fThreads) if (t.joinable()) t.join();
  fThreads.clear();
  fQueue.clear();
}

void ThreadPool::AddTask(Processor* obj, std::u32string input) {
  {
    const std::lock_guard<std::mutex> lg(fMutex);
    fBufferBytes += input.size()*sizeof(char32_t);
    fQueue.emplace_back(std::make_unique<task_t>(obj, std::move(input)));
    fWaitingTasks++;
  }
  fCV.notify_one();
}

void ThreadPool::AddTask(Processor* obj, std::vector<std::u32string>& input) {
  {
    const std::lock_guard<std::mutex> lg(fMutex);
    for (auto& s : input) {
      fBufferBytes += s.size()*sizeof(char32_t);
      fQueue.emplace_back(std::make_unique<task_t>(obj, std::move(s)));
      fWaitingTasks++;
    }
  }
  fCV.notify_one();
}

void ThreadPool::Run() {
  std::vector<std::unique_ptr<task_t>> tasks;
  struct timespec start, stop;
  std::map<TaskCode, double> benchmarks;
  TaskCode code;
  while (!fFinishNow) {
    std::unique_lock<std::mutex> lk(fMutex);
    fCV.wait(lk, [&]{return fWaitingTasks > 0 || fFinishNow;});
    if (fQueue.size() > 0 && !fFinishNow) {
      do {
        tasks.emplace_back(std::move(fQueue.front()));
        fQueue.pop_front();
      } while (fQueue.size() > 0 && fQueue.front()->code() == tasks.front()->code() && tasks.size() < fMaxPerPull);
      fWaitingTasks -= tasks.size();
      fRunningTasks += tasks.size();
      lk.unlock();
      code = static_cast<TaskCode>(tasks.front()->code());
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &start);
      for (auto& task : tasks) {
        std::invoke(&Processor::Process, task->obj, task->input);
        fBufferBytes -= task->input.size()*sizeof(char32_t);
        task.reset();
        fRunningTasks--;
      }
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &stop);
      tasks.clear();
      benchmarks[code] += timespec_subtract(stop, start);
    } else {
      lk.unlock();
    }
  }
  {
    const std::unique_lock<std::mutex> lk(fMutex_);
    for (auto& p : benchmarks) fBenchmarks[p.first] += p.second;
  }
}

