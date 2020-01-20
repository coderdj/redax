#ifndef _CCONTROL_HANDLER_HH_
#define _CCONTROL_HANDLER_HH_

#include <string>
#include <bsoncxx/document/value.hpp>

class MongoLog;
class Options;
class V2718;
class DDC10;
class V1495;

class CControl_Handler{
  
public:
  CControl_Handler(MongoLog *log, std::string procname);
  ~CControl_Handler();

  bsoncxx::document::value GetStatusDoc(std::string hostname);
  int DeviceArm(int run, Options *opts);
  int DeviceStart();
  int DeviceStop();

private:

  V2718 *fV2718;
  DDC10 *fDDC10;
  V1495 *fV1495;

  int fStatus;
  int fCurrentRun;
  int fBID;
  int fBoardHandle;
  std::string fProcname;
  Options *fOptions;
  MongoLog *fLog;
};

#endif
