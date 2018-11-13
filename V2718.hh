#ifndef _V2718_HH_
#define _V2718_HH_


#include <CAENVMElib.h>
#include "MongoLog.hh"
#include "Options.hh"

class V2718{
 
 public:
  V2718(MongoLog *log);
  ~V2718();



 int CrateInit(int led_trig, int m_veto, int n_veto, int pulser_freq, int s_in);
 
 private:
 MongoLog *fLog;
 Options *fOptions;
 
 bool b_mveto_on;
 bool b_nveto_on;
 bool b_startwithsin;
 bool b_led_on;
 int  i_pulser_Hz;
 bool bStarted;

 


};

#endif



