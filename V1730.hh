#ifndef _V1730_HH_
#define _V1730_HH_

#include "V1724.hh"

class V1730 : public V1724 {

public:
  V1730(std::shared_ptr<MongoLog>&, std::shared_ptr<Options>&);
  virtual ~V1730();

  virtual std::tuple<int, int, bool, uint32_t> UnpackEventHeader(std::u32string_view);
  virtual std::tuple<int64_t, int, uint16_t, std::u32string_view> UnpackChannelHeader(std::u32string_view, long, uint32_t, uint32_t, int, int);
private:

};

#endif
