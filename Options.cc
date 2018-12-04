#include "Options.hh"

Options::Options(MongoLog *log, std::string options_name, mongocxx::collection opts_collection,
		 std::string override_opts){
  bson_value = NULL;
  if(Load(options_name, opts_collection, override_opts)!=0)
    throw std::runtime_error("Can't initialize options class");
  fHelper = new DAXHelpers();
}

Options::~Options(){
  delete fHelper;
  if(bson_value != NULL)
    delete bson_value;
}

std::string Options::ExportToString(){
  std::string ret = bsoncxx::to_json(bson_options);
  return ret;
}

int Options::Load(std::string name, mongocxx::collection opts_collection,
		  std::string override_opts){
  // Try to pull doc from DB
  bsoncxx::stdx::optional<bsoncxx::document::value> trydoc;
  trydoc = opts_collection.find_one(bsoncxx::builder::stream::document{}<<
				    "name" << name.c_str() << bsoncxx::builder::stream::finalize);
  if(!trydoc){
    fLog->Entry("Failed to find your options file in DB", MongoLog::Warning);
    return -1;
  }
  if(bson_value != NULL)
    delete bson_value;
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
      if(sd) success += Override(*sd); // include_json.push_back(bsoncxx::to_json(*sd));
      else fLog->Entry("Possible improper run config. Check your options includes",
		       MongoLog::Warning);
    }
  }catch(...){}; // will catch if there are no includes, for example

  if(override_opts != "")
    success += Override(bsoncxx::from_json(override_opts));
  
  if(success!=0){
    fLog->Entry("Failed to override options doc with includes and overrides.", MongoLog::Warning);
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
    std::cout<<e.what()<<std::endl;
    
    // Some APIs autoconvert big ints to doubles. Why? I don't know.
    // But we can handle this here rather than chase those silly things
    // around in each implementation.
    try{
      return (long int)(bson_options[path.c_str()].get_double());
    }
    catch(const std::exception &e){
      return default_value;
    }
  }  
  return -1;
}

int Options::GetInt(std::string path, int default_value){

  try{
    return bson_options[path.c_str()].get_int32();
  }
  catch (const std::exception &e){
    //LOG
    std::cout<<"Exception: "<< e.what()<<std::endl;
    return default_value;    
  }
  return -1;  
}

std::string Options::GetString(std::string path, std::string default_value){
  try{
    return bson_options[path.c_str()].get_utf8().value.to_string();
  }
  catch (const std::exception &e){
    //LOG
    std::cout<< "Exception: "<< e.what()<<std::endl;
    return default_value;
  }  
  return "";
}

std::vector<BoardType> Options::GetBoards(std::string type, std::string hostname){
  std::vector<BoardType> ret;
  bsoncxx::array::view subarr = bson_options["boards"].get_array().value;
  
  for(bsoncxx::array::element ele : subarr){
    if(type != "" && type != ele["type"].get_utf8().value.to_string())
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
    bt.vme_address = fHelper->StringToHex(ele["vme_address"].get_utf8().value.to_string());
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


int Options::GetCrateOpt(CrateOptions &ret, std::string device){
  // I think we can just hack the above getters to allow dot notation
  // for a more robust solution to access subdocuments
  try{
    ret.s_in = bson_options["V2718"]["s_in"].get_int32().value;
    ret.pulser_freq = bson_options["V2718"]["pulser_freq"].get_int32().value;
    ret.muon_veto = bson_options["V2718"]["muon_veto"].get_int32().value;
    ret.neutron_veto = bson_options["V2718"]["neutron_veto"].get_int32().value;
    ret.led_trigger = bson_options["V2718"]["led_trigger"].get_int32().value;
  }catch(std::exception &E){
    std::cout<<"Exception getting ccontroller opts: "<<std::endl<<E.what()<<std::endl;
    return -1;
  }
  return 0;
}	

int Options::GetChannel(int bid, int cid){
  std::string boardstring = std::to_string(bid);
  try{
    return bson_options["channels"][boardstring][cid].get_int32().value;
  }
  catch(std::exception& e){
    std::cout<<"Failed to look up "<<bid<<" "<<cid<<std::endl;
    return -1;
  }
}
