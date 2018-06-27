#include "Options.hh"

Options::Options(){
  //if(LoadFile(defaultPath)!=0)
  //throw std::runtime_error("Can't initialize options class");
  fHelper = new DAXHelpers();
  bson_value = NULL;
}

Options::Options(std::string opts){
  if(Load(opts)!=0)
    throw std::runtime_error("Can't initialize options class");  
  fHelper = new DAXHelpers();
}

Options::~Options(){
  delete fHelper;
  if(bson_value != NULL)
    delete bson_value;
}

int Options::LoadFile(std::string path){
  /*
    Load a file and put the options into the internal bson 
    holder
  */
  
  try{
    std::ifstream t(path.c_str());
    std::string str((std::istreambuf_iterator<char>(t)),
		    std::istreambuf_iterator<char>());
    Load(str);
  }
  catch (const std::exception &e){
    std::cout<<e.what()<<std::endl;
    return -1;
  }
  return 0;  
}

std::string Options::ExportToString(){
  // This might be silly
  std::string ret = bsoncxx::to_json(bson_options);
  return ret;
}

int Options::Load(std::string opts){
  try{
    // This needs to be a pointer. Needs to stay in scope for as long as you might
    // want to see the view.
    bson_value = new bsoncxx::document::value(bsoncxx::from_json(opts).view());
    bson_options = bson_value->view();
  }
  catch (const bsoncxx::v_noabi::exception &e){
    std::cout<<"Failed to load file or parse JSON. Check that your JSON is valid."<<std::endl;
    std::cout<<e.what()<<std::endl;
    return -1;
  }
  catch(const std::exception &e){
    std::cout<<e.what()<<std::endl;
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
  
  //using bsoncxx::builder::concatenate;

  auto doc = document{};
  bsoncxx::document::value *new_value = new bsoncxx::document::value
    ( document{}<<bsoncxx::builder::concatenate_doc{bson_options}<<
      "override_doc" << bsoncxx::builder::stream::open_document<<
      bsoncxx::builder::concatenate_doc{override_opts}<<
      bsoncxx::builder::stream::close_document<<
      finalize);
  //builder<<concatenate(bson_options);
  //builder<<concatenate(override_opts);
  //doc<<finalize;
  
  // Create a new doc
  //bsoncxx::document::value *new_value = new bsoncxx::document::value(doc.extract());

  // Delete the original
  delete bson_value;

  // Set to new
  bson_value = new_value;
  bson_options = bson_value->view();

  return 0;  
}

int Options::GetInt(std::string path, int default_value){

  try{
    return bson_options["override_doc"][path.c_str()].get_int32();
  }
  catch (const std::exception &e){
    try{
      return bson_options[path.c_str()].get_int32();
    }
    catch (const std::exception &e){
      //LOG
      std::cout<<e.what()<<std::endl;
      return default_value;
    }
  }
  return -1;  
}

std::string Options::GetString(std::string path, std::string default_value){
  try{
    return bson_options["override_doc"][path.c_str()].get_utf8().value.to_string();
  }
  catch(const std::exception &e){
    try{
      return bson_options[path.c_str()].get_utf8().value.to_string();
    }
    catch (const std::exception &e){
      //LOG
      std::cout<<e.what()<<std::endl;
      return default_value;
    }
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
  std::cout<<"Got boards"<<std::endl;
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
