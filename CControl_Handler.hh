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
  CControl_Handler(std::shared_ptr<MongoLog>& log, std::string procname);
  ~CControl_Handler();

  bsoncxx::document::value GetStatusDoc(std::string hostname);
  int DeviceArm(int run, std::shared_ptr<Options>& opts);
  int DeviceStart();
  int DeviceStop();

private:

  std::unique_ptr<V2718> fV2718;
  std::unique_ptr<DDC10> fDDC10;
  std::unique_ptr<V1495> fV1495;

  int fStatus;
  int fCurrentRun;
  int fBID;
  int fBoardHandle;
  std::string fProcname;
  std::shared_ptr<Options> fOptions;
  std::shared_ptr<MongoLog> fLog;
};

#endif
