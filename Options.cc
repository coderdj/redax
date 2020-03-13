#include "Options.hh"
#include "DAXHelpers.hh"
#include "MongoLog.hh"

#include <mongocxx/uri.hpp>
#include <mongocxx/database.hpp>
#include <bsoncxx/array/view.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>

Options::Options(MongoLog *log, std::string options_name,
          std::string suri, std::string dbname, std::string override_opts){
  bson_value = NULL;
  fLog = log;
  mongocxx::uri uri{suri};
  fClient = mongocxx::client{uri};
  fDAC_collection = fClient[dbname]["dac_calibration"];
  mongocxx::collection opts_collection = fClient[dbname]["options"];
  if(Load(options_name, opts_collection, override_opts)!=0)
    throw std::runtime_error("Can't initialize options class");
}

Options::~Options(){
  if(bson_value != NULL) {
    delete bson_value;
    bson_value = NULL;
  }
}

std::string Options::ExportToString(){
  std::string ret = bsoncxx::to_json(bson_options);
  return ret;
}

int Options::Load(std::string name, mongocxx::collection& opts_collection,
	std::string override_opts){
  // Try to pull doc from DB
  bsoncxx::stdx::optional<bsoncxx::document::value> trydoc;
  trydoc = opts_collection.find_one(bsoncxx::builder::stream::document{}<<
				    "name" << name.c_str() << bsoncxx::builder::stream::finalize);
  if(!trydoc){
    fLog->Entry(MongoLog::Warning, "Failed to find your options file '%s' in DB", name);
    return -1;
  }
  if(bson_value != NULL) {
    delete bson_value;
    bson_value = NULL;
  }
  bson_value = new bsoncxx::document::value((*trydoc).view());
  bson_options = bson_value->view();
  
  // Pull all subdocuments
  int success = 0;
  try{
    bsoncxx::array::view include_array = (*trydoc).view()["includes"].get_array().value;
    for(bsoncxx::array::element ele : include_array){
      auto sd = opts_collection.find_one(bsoncxx::builder::stream::document{} <<
					 "name" << ele.get_utf8().value.to_string() <<
					 bsoncxx::builder::stream::finalize);
      if(sd)
	success += Override(*sd); // include_json.push_back(bsoncxx::to_json(*sd));
      else
	fLog->Entry(MongoLog::Warning, "Possible improper run config. Check your options includes");
    }
  }catch(...){}; // will catch if there are no includes, for example

  if(override_opts != "")
    success += Override(bsoncxx::from_json(override_opts));

  if(success!=0){
    fLog->Entry(MongoLog::Warning, "Failed to override options doc with includes and overrides.");
    return -1;
  }


  return 0;
}

int Options::Override(bsoncxx::document::view override_opts){

  // Here's the best way I can find to do this. We create a new doc, which
  // is a concatenation of the two old docs (original first). Then we
  // use the concatenation object to initialize a 'new' value for the
  // combined doc and delete the orginal. A new view will point to the new value.

  using bsoncxx::builder::stream::document;
  using bsoncxx::builder::stream::finalize;    

  // auto doc = document{};
  bsoncxx::document::value *new_value = new bsoncxx::document::value
    ( document{}<<bsoncxx::builder::concatenate_doc{bson_options}<<
      // "override_doc" << bsoncxx::builder::stream::open_document<<
      bsoncxx::builder::concatenate_doc{override_opts}<<
      // bsoncxx::builder::stream::close_document<<
      finalize);
  
  // Delete the original
  delete bson_value;

  // Set to new
  bson_value = new_value;
  bson_options = bson_value->view();

  return 0;  
}

long int Options::GetLongInt(std::string path, long int default_value){
  try{
    return bson_options[path.c_str()].get_int64();
  }
  catch (const std::exception &e){
    // Some APIs autoconvert big ints to doubles. Why? I don't know.
    // But we can handle this here rather than chase those silly things
    // around in each implementation.
    try{
      return (long int)(bson_options[path.c_str()].get_double());
    }
    catch(const std::exception &e){
      fLog->Entry(MongoLog::Local, "Using default value for %s", path.c_str());
      return default_value;
    }
  }  
  return -1;
}

double Options::GetDouble(std::string path, double default_value) {
  try{
    return bson_options[path].get_double();
  } catch (const std::exception& e) {
    fLog->Entry(MongoLog::Local, "Using default value for %s", path.c_str());
    return default_value;
  }
}

int Options::GetInt(std::string path, int default_value){

  try{
    return bson_options[path.c_str()].get_int32();
  }
  catch (const std::exception &e){
    //LOG
    fLog->Entry(MongoLog::Local, "Using default value for %s", path.c_str());
    return default_value;
  }
  return -1;  
}

int Options::GetNestedInt(std::string path, int default_value){
  // Parse string
  std::vector<std::string> fields;
  std::stringstream ss(path);
  while( ss.good() ){
    std::string substr;
    getline( ss, substr, '.' );
    fields.push_back( substr );
  }
  try{
    auto val = bson_options[fields[0].c_str()];
    for(unsigned int i=1; i<fields.size(); i++)
      val = val[fields[i].c_str()];
    return val.get_int32();
  }catch(const std::exception &e){
    fLog->Entry(MongoLog::Local, "Using default value for %s",path.c_str());
    return default_value;
  }
  return 0;
}

std::string Options::GetString(std::string path, std::string default_value){
  try{
    return bson_options[path.c_str()].get_utf8().value.to_string();
  }
  catch (const std::exception &e){
    //LOG
    fLog->Entry(MongoLog::Local, "Using default value for %s", path.c_str());
    return default_value;
  }  
  return "";
}

std::vector<BoardType> Options::GetBoards(std::string type, std::string hostname){
  std::vector<BoardType> ret;
  bsoncxx::array::view subarr = bson_options["boards"].get_array().value;

  std::vector <std::string> types;
  if(type == "V17XX")
    types = {"V1724", "V1730", "V1724_MV"};
  else
    types.push_back(type);
  
  for(bsoncxx::array::element ele : subarr){
    std::string btype = ele["type"].get_utf8().value.to_string();
    if(!std::count(types.begin(), types.end(), btype))
      continue;
    try{
      if(ele["host"].get_utf8().value.to_string() != hostname)
	continue;
    }
    catch(const std::exception &e){
      // If there is no host field then no biggie. Assume we have just 1 host.
    };
    BoardType bt;
    bt.link = ele["link"].get_int32();
    bt.crate = ele["crate"].get_int32();
    bt.board = ele["board"].get_int32();
    bt.type = ele["type"].get_utf8().value.to_string();    
    bt.vme_address = DAXHelpers::StringToHex(ele["vme_address"].get_utf8().value.to_string());
    ret.push_back(bt);
  }
  
  return ret;
}
    
std::vector<RegisterType> Options::GetRegisters(int board){
  std::vector<RegisterType> ret;
  
  bsoncxx::array::view regarr = bson_options["registers"].get_array().value;
  for(bsoncxx::array::element ele : regarr){
    if(board != ele["board"].get_int32() && ele["board"].get_int32() != -1)
      continue;
    RegisterType rt;
    rt.board = ele["board"].get_int32();
    rt.reg = ele["reg"].get_utf8().value.to_string();
    rt.val = ele["val"].get_utf8().value.to_string();

    ret.push_back(rt);
  }
  return ret;
}

std::vector<u_int16_t> Options::GetThresholds(int board) {
  std::vector<u_int16_t> thresholds;
  u_int16_t default_threshold = 0xA;
  try{
    for (auto& val : bson_options["thresholds"][std::to_string(board)].get_array().value)
      thresholds.push_back(val.get_int32().value);
    return thresholds;
  }
  catch(std::exception& e){
    fLog->Entry(MongoLog::Local, "Using default thresholds for %i", board);
    return std::vector<u_int16_t>(16, default_threshold);
  }
}

int Options::GetCrateOpt(CrateOptions &ret){
  // I think we can just hack the above getters to allow dot notation
  // for a more robust solution to access subdocuments
  try{
    try{
      ret.pulser_freq = float(bson_options["V2718"]["pulser_freq"].get_int32().value);
    } catch(std::exception& e) {
      ret.pulser_freq = bson_options["V2718"]["pulser_freq"].get_double().value;
    }
    ret.s_in = float(bson_options["V2718"]["s_in"].get_int32().value);
    ret.muon_veto = bson_options["V2718"]["muon_veto"].get_int32().value;
    ret.neutron_veto = bson_options["V2718"]["neutron_veto"].get_int32().value;
    ret.led_trigger = bson_options["V2718"]["led_trigger"].get_int32().value;
  }catch(std::exception &E){
    fLog->Entry(MongoLog::Local, "Exception getting ccontroller opts: %s", E.what());
    return -1;
  }
  return 0;
}

int16_t Options::GetChannel(int bid, int cid){
  std::string boardstring = std::to_string(bid);
  try{
    return bson_options["channels"][boardstring][cid].get_int32().value;
  }
  catch(std::exception& e){
    fLog->Entry(MongoLog::Local, "Failed to look up board %i ch %i", bid, cid);
    return -1;
  }
}

int Options::GetHEVOpt(HEVOptions &ret){
  try{
    ret.signal_threshold = bson_options["DDC10"]["signal_threshold"].get_int32().value;
    ret.sign = bson_options["DDC10"]["sign"].get_int32().value;
    ret.rise_time_cut = bson_options["DDC10"]["rise_time_cut"].get_int32().value;
    ret.inner_ring_factor = bson_options["DDC10"]["inner_ring_factor"].get_int32().value;
    ret.outer_ring_factor = bson_options["DDC10"]["outer_ring_factor"].get_int32().value;
    ret.integration_threshold = bson_options["DDC10"]["integration_threshold"].get_int32().value;
    ret.parameter_0 = bson_options["DDC10"]["parameter_0"].get_int32().value;
    ret.parameter_1 = bson_options["DDC10"]["parameter_1"].get_int32().value;
    ret.parameter_2 = bson_options["DDC10"]["parameter_2"].get_int32().value;
    ret.parameter_3 = bson_options["DDC10"]["parameter_3"].get_int32().value;
    ret.window = bson_options["DDC10"]["window"].get_int32().value;
    ret.prescaling = bson_options["DDC10"]["prescaling"].get_int32().value;
    ret.component_status = bson_options["DDC10"]["component_status"].get_int32().value;
    ret.width_cut = bson_options["DDC10"]["width_cut"].get_int32().value;
    ret.delay = bson_options["DDC10"]["delay"].get_int32().value;

    ret.address = bson_options["DDC10"]["address"].get_utf8().value.to_string();
    ret.required = bson_options["DDC10"]["required"].get_utf8().value.to_string();
  }catch(std::exception &E){
    fLog->Entry(MongoLog::Local, "Exception getting DDC10 opts: %s",E.what());
    return -1;
  }
  return 0;
}

int Options::GetDAC(std::map<int, std::map<std::string, std::vector<double>>>& board_dacs,
                    std::vector<int>& bids) {
  board_dacs.clear();
  std::map<std::string, std::vector<double>> defaults {
                {"slope", std::vector<double>(16, -0.2695)},
                {"yint", std::vector<double>(16, 17169)}};
  // let's provide a default
  board_dacs[-1] = defaults;
  std::map<std::string, std::vector<double>> this_board_dac{
    {"slope", std::vector<double>(16)},
    {"yint", std::vector<double>(16)}};
  int ret(0);
  auto sort_order = bsoncxx::builder::stream::document{} <<
    "_id" << -1 << bsoncxx::builder::stream::finalize;
  auto opts = mongocxx::options::find{};
  opts.sort(sort_order.view());
  auto cursor = fDAC_collection.find({}, opts);
  auto doc = cursor.begin();
  if (doc == cursor.end()) {
    fLog->Entry(MongoLog::Local, "No baseline calibrations? You must be new");
    return -1;
  }
/* doc should look like this:
 *{ run : 000042,
 * bid : {
 *              slope : [ch0, ch1, ch2, ...],
 *              yint : [ch0, ch1, ch2, ...],
 *         },
 *         ...
 * }
 */
  for (auto bid : bids) {
    if ((*doc).find(std::to_string(bid)) == (*doc).end()) {
      board_dacs[bid] = defaults;
      continue;
    }
    for (auto& kv : this_board_dac) { // (string, vector<double>)
      kv.second.clear();
      for(auto& val : (*doc)[std::to_string(bid)][kv.first].get_array().value)
	kv.second.push_back(val.get_double());
    }
    board_dacs[bid] = this_board_dac;
  }
  return ret;
}

void Options::UpdateDAC(std::map<int, std::map<std::string, std::vector<double>>>& all_dacs){
  using namespace bsoncxx::builder::stream;
  std::string run_id = GetString("run_identifier", "default");
  fLog->Entry(MongoLog::Local, "Saving DAC calibration");
  auto search_doc = document{} << "run" << run_id << finalize;
  auto update_doc = document{};
  update_doc<< "$set" << open_document << "run" << run_id;
  for (auto& bid_map : all_dacs) { // (bid, map<string, vector>)
    update_doc << std::to_string(bid_map.first) << open_document;
    for(auto& str_vec : bid_map.second){ // (string, vector)
      update_doc << str_vec.first << open_array <<
        [&](array_context<> arr){
        for (auto& val : str_vec.second) arr << val;
        } << close_array;
    }
    update_doc << close_document;
  }
  update_doc << close_document;
  auto write_doc = update_doc<<finalize;
  mongocxx::options::update options;
  options.upsert(true);
  fDAC_collection.update_one(search_doc.view(), write_doc.view(), options);
  return;
}

void SaveBenchmarks(std::map<int, long>& buffer_counter, long bytes,
    double proc_time_us, double comp_time_us, long tid) {

}
