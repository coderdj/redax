#ifndef _CCONTROL_HANDLER_HH_
#define _CCONTROL_HANDLER_HH_

#include "Options.hh"
#include "MongoLog.hh"

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
  /*
    Does all the DAQ-wide run start stuff. DDC-10, V1495, and V2718
  */
  
public:
  CControl_Handler();
  ~CControl_Handler();

  void ProcessCommand(std::string command, std::string detector,
		      int run, std::string options="");

  bsoncxx::document::value GetStatusDoc(std::string hostname);

private:

  std::map<int, modules> fActiveRuns;

};

#endif
