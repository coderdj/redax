#ifndef _V2718_HH_
#define _V2718_HH_

#include "Options.hh"

class MongoLog;

class V2718{
public:
  V2718(std::shared_ptr<MongoLog>&);
  virtual ~V2718();

  virtual int CrateInit(CrateOptions c_opts, int link, int crate);
  virtual int SendStartSignal();
  virtual int SendStopSignal(bool end=true); 

  CrateOptions GetCrateOptions(){ return fCopts;};
  int GetHandle(){return fBoardHandle;};

protected:
  int fBoardHandle;
  CrateOptions fCopts;
  int fCrate, fLink;
  std::shared_ptr<MongoLog> fLog;

};

#endif
