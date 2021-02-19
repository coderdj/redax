#ifndef _V2718_HH_
#define _V2718_HH_

#include "Options.hh"

class MongoLog;

class V2718{
public:
  V2718(std::shared_ptr<MongoLog>&, CrateOptions);
  virtual ~V2718();

  virtual int Init(int, int);
  virtual int SendStartSignal();
  virtual int SendStopSignal(bool end=true);

  CrateOptions GetCrateOptions(){ return fCopts;};
  int GetHandle(){return fBoardHandle;};

protected:
  int fBoardHandle;
  CrateOptions fCopts;
  std::shared_ptr<MongoLog> fLog;

};

#endif
