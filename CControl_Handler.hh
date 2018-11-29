#ifndef _CCONTROL_HANDLER_HH_
#define _CCONTROL_HANDLER_HH_

#include "Options.hh"
#include "MongoLog.hh"
#include "V2718.hh"
#include <thread>

class V1495;
class DDC10;

class CControl_Handler{
  
public:
  CControl_Handler(MongoLog *log, std::string procname);
  ~CControl_Handler();

  void ProcessCommand(std::string command, std::string detector,
		      int run, std::string options="");

  bsoncxx::document::value GetStatusDoc(std::string hostname);
  int DeviceArm(int run, std::string options="");
  int DeviceStart();
  int DeviceStop();

private:

  V2718 *fV2718;
  V1495 *fV1495;
  DDC10 *fDDC10;
  
  std::string fProcname;
  Options *fOptions;
  MongoLog *fLog;
};

#endif
