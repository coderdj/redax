#include <bsoncxx/document/view.hpp>
#include <bsoncxx/json.hpp>
//#include <bsoncxx/document/value.hpp>
//#include <bsoncxx/stdx/optional.hpp>
#include "V1724.hh"


class DAQController{

public:
  DAQController();
  ~DAQController();

  int InitializeElectronics(bsoncxx::document::view options);
  void End();
  
  static unsigned int StringToHex(std::string str);
private:
  vector <V1724*> fDigitizers;
  bsoncxx::document::view fOptions;
  
};
