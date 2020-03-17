#ifndef _OPTIONS_HH_
#define _OPTIONS_HH_

#include <string>
#include <vector>
#include <fstream>
#include <streambuf>
#include <iostream>
#include <stdexcept>
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
  int board;
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
  Options(MongoLog *log, std::string name, std::string hostname, std::string suri,
  	std::string dbname, std::string override_opts);
  ~Options();

  int Load(std::string name, mongocxx::collection& opts_collection, std::string override_opts);
  int Override(bsoncxx::document::view override_opts);
  std::string ExportToString();
  
  int GetInt(std::string key, int default_value=-1);
  long int GetLongInt(std::string key, long int default_value=-1);
  double GetDouble(std::string key, double default_value=-1);
  std::string GetString(std::string key, std::string default_value="");

  std::vector<BoardType> GetBoards(std::string type="", std::string hostname="DEFAULT");
  std::vector<RegisterType> GetRegisters(int board=-1);
  int GetDAC(std::map<int, std::map<std::string, std::vector<double>>>& board_dacs, std::vector<int>& bids);
  int GetCrateOpt(CrateOptions &ret);
  int GetHEVOpt(HEVOptions &ret);
  int16_t GetChannel(int bid, int cid);
  int GetNestedInt(std::string path, int default_value);
  std::vector<u_int16_t> GetThresholds(int board);

  void UpdateDAC(std::map<int, std::map<std::string, std::vector<double>>>&);
  void SaveBenchmarks(std:map<std::string, long>&, std::map<int, long>& double, double);
private:
  mongocxx::client fClient;
  bsoncxx::document::view bson_options;
  bsoncxx::document::value *bson_value;
  MongoLog *fLog;
  mongocxx::collection fDAC_collection;
  std::string fDBname;
  std::string fHostname;
};

#endif
