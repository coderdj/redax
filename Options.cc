#include "Options.hh"

Options::Options(){
  if(LoadFile(defaultPath)!=0)
    throw std::runtime_error("Can't initialize options class");
  fHelper = new DAXHelpers();
}

Options::Options(std::string opts){
  if(Load(opts)!=0)
    throw std::runtime_error("Can't initialize options class");  
  fHelper = new DAXHelpers();
}

Options::~Options(){
  delete fHelper;
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

int Options::Load(std::string opts){
  try{
    bson_options = bsoncxx::from_json(opts).view();
  }
  catch (const bsoncxx::v_noabi::exception &e){
    std::cout<<"Failed to load file or parse JSON. Check that your JSON is valid."<<std::endl;
    std::cout<<e.what()<<std::endl;
    return -1;
  }
  catch(const std::exception &e){
    std::cout<<e.what()<<std::endl;
    return -1;
  }
  return 0;
}

int Options::GetInt(std::string path){

  try{
    return bson_options[path].get_int32();
  }
  catch (const std::exception &e){
    //LOG
    std::cout<<e.what()<<std::endl;
  }
  return -1;  
}

std::string Options::GetString(std::string path){

  try{
    return bson_options[path].get_utf8().value.to_string();
  }
  catch (const std::exception &e){
    //LOG
    std::cout<<e.what()<<std::endl;
  }
  return "";
}

std::vector<BoardType> Options::GetBoards(std::string type){
  std::vector<BoardType> ret;
  bsoncxx::array::view subarr = bson_options["boards"].get_array().value;
  
  for(bsoncxx::array::element ele : subarr){
    if(type != "" && type != ele["type"].get_utf8().value.to_string())
      continue;
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
