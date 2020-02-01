#ifndef _V2718_HH_
#define _V2718_HH_

#include "Options.hh"

class MongoLog;

class V2718{
 
public:
  V2718(MongoLog *log);
  ~V2718();
  
  int CrateInit(CrateOptions c_opts, int link, int crate);
  int SendStartSignal();
  int SendStopSignal(bool end=true); 
  
  CrateOptions GetCrateOptions(){ return fCopts;};
  int GetHandle(){return fBoardHandle;};

protected:
  int fBoardHandle;

private:
  
  CrateOptions fCopts;
  int fCrate, fLink;
  MongoLog *fLog;

};

#endif
