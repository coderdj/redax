#ifndef _CCONTROL_HANDLER_HH_
#define _CCONTROL_HANDLER_HH_

#include "Options.hh"
#include "MongoLog.hh"
#include "V2718.hh"
#include <thread>

class V2718;
class V1495;
class DDC10;

struct modules{
  int number;
  V2718 *v2718 = NULL;
  V1495 *v1495 = NULL;
  DDC10 *ddc10 = NULL;
};


class CControl_Handler{
  
public:
  CControl_Handler(MongoLog *log);
  ~CControl_Handler();

  void ProcessCommand(std::string command, std::string detector,
		      int run, std::string options="");

  bsoncxx::document::value GetStatusDoc(std::string hostname);
  int DeviceArm(int run, std::string options="");
  int DeviceStart();
  int DeviceStop();

private:

  std::map<int, modules> fActiveRuns;  
  Options *fOptions;
  MongoLog *fLog;
};

#endif
