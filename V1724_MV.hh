#ifndef _V1724_MV_HH_
#define _V1724_MV_HH_

#include "V1724.hh"

class V1724_MV : public V1724 {

public:
  V1724_MV(std::shared_ptr<ThreadPool>&, std::shared_ptr<Processor>&, std::shared_ptr<Options>&, std::shared_ptr<MongoLog>&);
  virtual ~V1724_MV();

  virtual std::tuple<int64_t, int, uint16_t, std::u32string_view> UnpackChannelHeader(std::u32string_view, long, uint32_t, uint32_t, int, int);
};

#endif // _V1724_MV_HH_ defined
