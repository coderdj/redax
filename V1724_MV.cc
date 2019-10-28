#include "V1724_MV.hh"

V1724_MV::V1724_MV(MongoLog *log, Options *options)
  :V1724(log, options){
  DataFormatDefinition["channel_header_size"] = 0;
}

V1724_MV::~V1724_MV(){}
