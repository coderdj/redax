#include "V1724_MV.hh"

class V1724_ZLE : public V1724_MV {
  public:
    V1724_ZLE(std::shared_ptr<MongoLog>& log, std::shared_ptr<Options>& opts) : 
      V1724_MV(log, opts) {}
    virtual ~V1724_ZLE();
};
