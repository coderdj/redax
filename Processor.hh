#ifndef _PROCESSOR_HH_
#define _PROCESSOR_HH_

#include <string_view>
#include <memory>

class ThreadPool;
class Options;
class MongoLog;

// Base class to support thread pool, also stores some common members
class Processor {
  public:
    Processor(std::shared_ptr<ThreadPool>& tp, std::shared_ptr<Processor>& next, std::shared_ptr<Options>& options, std::shared_ptr<MongoLog>& log) : fTP(tp), fNext(next), fOptions(options), fLog(log) {}
    virtual ~Processor();
    virtual void Process(std::string_view) {}
    virtual void Process(std::u32string_view) {}
    virtual void End() {}

  protected:
    std::shared_ptr<ThreadPool> fTP;
    std::shared_ptr<Processor> fNext;
    std::shared_ptr<Options> fOptions;
    std::shared_ptr<MongoLog> fLog;
};

#endif // _PROCESSOR_HH_ defined
