#ifndef _F2718_HH_
#define _F2718_HH_

#include "V2718.hh"


class F2718 : public V2718 { // Fax-compatible V2718;
public:
  F2718(MongoLog*);
  virtual ~F2718();

  virtual int CrateInit(CrateOptions, int, int) {}
  virtual int SendStartSignal();
  virtual int SendStopSignal(bool=true);

private:
  int SendSignal(const std::string&);
};

#endif // _F2718_HH_ defined
