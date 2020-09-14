#ifndef _OPTIONS_HH_
#define _OPTIONS_HH_

#include <string>
#include <vector>
#include <stdexcept>
#include <memory>
#include <map>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/document/value.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/client.hpp>

struct BoardType{
  int link;
  int crate;
  int board;
  std::string type;
  std::string host;
  unsigned int vme_address;
};

struct RegisterType{
  std::string reg;
  std::string val;
};

struct CrateOptions{
  float pulser_freq;
  int neutron_veto;
  int muon_veto;
  int led_trigger;
  int s_in;
};

struct HEVOptions{
    int signal_threshold;
    int sign;
    int rise_time_cut;
    int inner_ring_factor;
    int outer_ring_factor;
    int integration_threshold;
    int parameter_0;
    int parameter_1;
    int parameter_2;
    int parameter_3;
    int window;
    int prescaling;
    int component_status;
    int width_cut;
    int delay;
    std::string address;
    std::string required;

};

class MongoLog;

class Options{

public:
  Options(std::shared_ptr<MongoLog>&, std::string, std::string, std::string, std::string, std::string);
  ~Options();

  int GetInt(const std::string&, int=-1);
  long int GetLongInt(const std::string&, long int=-1);
  double GetDouble(const std::string&, double=-1);
  std::string GetString(const std::string&, std::string="");

  std::vector<BoardType> GetBoards(const std::string&);
  std::vector<RegisterType> GetRegisters(int, bool=false);
  int GetDAC(std::map<int, std::map<std::string, std::vector<double>>>&, std::vector<int>&);
  int GetCrateOpt(CrateOptions &ret);
  int GetHEVOpt(HEVOptions &ret);
  int16_t GetChannel(int, int);
  int GetNestedInt(const std::string&, int);
  std::vector<uint16_t> GetThresholds(int);
  int GetProcessingThreads();

  void UpdateDAC(std::map<int, std::map<std::string, std::vector<double>>>&);
  void SaveBenchmarks(std::map<std::string, long>&, std::map<int, long>&, double, double, double, double);

  std::string fHostname;

private:
  int Load(std::string, mongocxx::collection&, std::string);
  int Override(bsoncxx::document::view);
  mongocxx::client fClient;
  bsoncxx::document::view bson_options;
  bsoncxx::document::value *bson_value;
  std::shared_ptr<MongoLog> fLog;
  mongocxx::collection fDAC_collection;
  std::string fDBname;
  std::string fDetector;
};

#endif
