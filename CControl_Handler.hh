#ifndef _CCONTROL_HANDLER_HH_
#define _CCONTROL_HANDLER_HH_

#include "DAQController.hh"

class V2718;
class DDC10;
class V1495;

class CControl_Handler : public DAQController{
public:
  CControl_Handler(std::shared_ptr<MongoLog>&, std::string);
  virtual ~CControl_Handler();

  virtual void StatusUpdate(mongocxx::collection*);
  virtual int Arm(std::shared_ptr<Options>&);
  virtual int Start();
  virtual int Stop();

private:

  std::unique_ptr<V2718> fV2718;
  std::unique_ptr<DDC10> fDDC10;
  std::unique_ptr<V1495> fV1495;

  int fStatus;
  int fCurrentRun;
  int fBID;
  int fBoardHandle;
};

#endif // _CCONTROL_HANDLER_HH_ defined
