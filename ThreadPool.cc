#include "ThreadPool.hh"
#include "Processor.hh"
#include <functional>
#include <ctime>
#include <iostream>
#include <cmath>

inline double timespec_subtract(const struct timespec& a, const struct timespec& b) {
  return (a.tv_sec - b.tv_sec)*1e6 + (a.tv_nsec - b.tv_nsec)*0.001;
}

ThreadPool::ThreadPool(int num_threads, bytes_per_pull) {
  fFinishNow = false;
  fWaitingTasks = fRunningTasks = 0;
  fBufferBytes = 0;
  fThreads.reserve(num_threads);
  for (int i = 0; i < num_threads; i++) fThreads.emplace_back(&ThreadPool::Run, this);
  fBytesPerPull = bytes_per_pull;
}

ThreadPool::~ThreadPool() {
  fWaitingTasks = 0;
  Kill();
  for (auto& t : fThreads) if (t.joinable()) t.join();
  fThreads.clear();
  fQueue.clear();
  for (auto& p : fBenchmarks)
    std::cout<<"BM " <<p.first<<": "<<p.second<<" ("<<fCounter[p.first]<<")"<<std::endl;
}

void ThreadPool::AddTask(Processor* obj, std::u32string input) {
  {
    const std::lock_guard<std::mutex> lg(fMutex);
    fBufferBytes += input.size()*sizeof(char32_t);
    fQueue.emplace_back(new task_t{obj, std::move(input)});
    fCounter[fQueue.back()->code()]++;
    fWaitingTasks++;
  }
  fCV.notify_one();
}

void ThreadPool::AddTask(Processor* obj, std::vector<std::u32string>& input) {
  {
    const std::lock_guard<std::mutex> lg(fMutex);
    for (auto& s : input) {
      fBufferBytes += s.size()*sizeof(char32_t);
      fQueue.emplace_back(new task_t{obj, std::move(s)});
      fCounter[fQueue.back()->code()]++;
      fWaitingTasks++;
    }
  }
  fCV.notify_one();
}

void ThreadPool::Run() {
  std::vector<std::unique_ptr<task_t>> tasks;
  struct timespec start, stop;
  std::map<TaskCode, double> benchmarks;
  std::map<TaskCode, long> counter;
  int bytes_this_pull;
  TaskCode code;
  while (!fFinishNow) {
    std::unique_lock<std::mutex> lk(fMutex);
    fCV.wait(lk, [&]{return fWaitingTasks > 0 || fFinishNow;});
    if (fQueue.size() > 0 && !fFinishNow) {
      do {
        tasks.emplace_back(std::move(fQueue.front()));
        fQueue.pop_front();
        bytes_this_pull += tasks.back()->input.size()*sizeof(char32_t);
        fCounter[tasks.back()->code()]--;
      } while (fQueue.size() > 0 && bytes_this_pull < fBytesPerPull);
      fWaitingTasks -= tasks.size();
      fRunningTasks += tasks.size();
      lk.unlock();
      for (auto& task : tasks) {
        code = static_cast<TaskCode>(task->code());
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &start);
        counter[code]++;
        std::invoke(&Processor::Process, task->obj, task->input);
        fBufferBytes -= task->input.size()*sizeof(char32_t);
        task.reset();
        fRunningTasks--;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &stop);
        benchmarks[code] += timespec_subtract(stop, start);
      }
      bytes_this_pull = 0;
      tasks.clear();
    } else {
      lk.unlock();
    }
  }
  {
    const std::lock_guard<std::mutex> lk(fMutex_);
    for (auto& p : benchmarks) fBenchmarks[p.first] += p.second;
    for (auto& p : counter) fCounter[p.first] += p.second;
  }
}

void ThreadPool::PrintStatus() {
  std::cout<<"Running: "<<fRunningTasks.load()<<" | Waiting: "<<fWaitingTasks.load()
    <<" | Bytes: "<<int(std::log2(fBufferBytes.load()))<<" | Counters: ";
  for (auto p : fCounter) std::cout<<p.first<<' '<<p.second<<',';
  std::cout<<std::endl;
}

