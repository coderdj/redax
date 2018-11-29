#ifndef _OPTIONS_HH_
#define _OPTIONS_HH_

#include <string>
#include <vector>
#include <fstream>
#include <streambuf>
#include <iostream>
#include <stdexcept>
#include <bsoncxx/array/view.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include "DAXHelpers.hh"

struct BoardType{
  int link;
  int crate;
  int board;
  std::string type;
  std::string device;
  unsigned int vme_address;
};

struct RegisterType{
  int board;
  std::string reg;
  std::string val;

};

struct CrateOptions{
  int pulser_freq;
  int neutron_veto;
  int muon_veto;
  int led_trigger;
  int s_in;
};



class Options{

public:
  Options();
  Options(std::string opts, std::vector<std::string>include_opts={});
  ~Options();

  int LoadFile(std::string path);
  int Load(std::string opts, std::vector<std::string>include_opts={});
  
  int GetInt(std::string key, int default_value=-1);
  long int GetLongInt(std::string key, long int default_value=-1);
  std::string GetString(std::string key, std::string default_value="");

  std::vector<BoardType> GetBoards(std::string type="", std::string hostname="DEFAULT");
  std::vector<RegisterType> GetRegisters(int board=-1);
  int GetCrateOpt(CrateOptions &ret, std::string device="");


  std::string ExportToString();
  int Override(bsoncxx::document::view override_opts);

  int GetChannel(int bid, int cid){ return cid;};
  
private:
  std::string defaultPath = "defaults/daxOptions.json";
  DAXHelpers *fHelper;
  bsoncxx::document::view bson_options;
  bsoncxx::document::value *bson_value;
};

#endif
