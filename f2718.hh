#ifndef _F2718_HH_
#define _F2718_HH_

#include "V2718.hh"

class f2718 : public V2718 {
  public:
    f2718(std::shared_ptr<MongoLog>& log, CrateOptions opt);
    virtual ~f2718();

    virtual int SendStartSignal() {return 0;}
    virtual int SendStopSignal(bool) {return 0;}

  protected:
    virtual int Init() {return 0;}
};

#endif // _F2718_HH_ defined
