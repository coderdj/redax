#include "ThreadPool.hh"
#include "Processor.hh"
#include <functional>
#include <ctime>
#include <iostream>

inline double timespec_subtract(const struct timespec& a, const struct timespec& b) {
  return (a.tv_sec - b.tv_sec)*1e6 + (a.tv_nsec - b.tv_nsec)*0.001;
}

ThreadPool::ThreadPool(int num_threads) {
  fFinishNow = false;
  fWaitingTasks = fRunningTasks = 0;
  fBufferBytes = 0;
  fThreads.reserve(num_threads);
  for (int i = 0; i < num_threads; i++) fThreads.emplace_back(&ThreadPool::Run, this);
  fMaxPerPull = {
    {TaskCode::UnpackDatapacket, 2},
    {TaskCode::UnpackChannel, 32},
    {TaskCode::CompressChunk, 2},
    {TaskCode::GenerateWaveform, 2}
  };
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
    fQueue.emplace_back(new task_t{obj, std::move(input)});
    fCounter[fQueue.back().code()]++;
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
      fCounter[fQueue.back().code()]++;
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
  auto max_per_pull = fMaxPerPull; // copy for thread safety
  TaskCode code;
  while (!fFinishNow) {
    std::unique_lock<std::mutex> lk(fMutex);
    fCV.wait(lk, [&]{return fWaitingTasks > 0 || fFinishNow;});
    if (fQueue.size() > 0 && !fFinishNow) {
      do {
        tasks.emplace_back(std::move(fQueue.front()));
        fQueue.pop_front();
      } while (fQueue.size() > 0 && tasks.size() < max_per_pull[tasks.front()->code()] \
          && fQueue.front()->code() == tasks.front()->code());
      fWaitingTasks -= tasks.size();
      fRunningTasks += tasks.size();
      fCounter[tasks.front().code()] -= tasks.size();
      counter[code] += tasks.size();
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
      benchmarks[code] += timespec_subtract(stop, start);
      tasks.clear();
    } else {
      lk.unlock();
    }
  }
  {
    const std::unique_lock<std::mutex> lk(fMutex_);
    for (auto& p : benchmarks) fBenchmarks[p.first] += p.second;
  }
}

void ThreadPool::PrintStatus() {
  std::cout<<"Running: "<<fRunningTasks.load()<<"| Waiting: "<<fWaitingTasks.load()
    <<"Bytes: "<<fBufferBytes.load()<<"| Counters: ";
  for (auto p : fCounter) std::cout<<p.first<<' '<<p.second<<'|';
  std::cout<<std::endl;
}

