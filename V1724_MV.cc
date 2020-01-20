#include "V1724_MV.hh"
#include "MongoLog.hh"
#include "Options.hh"

V1724_MV::V1724_MV(MongoLog *log, Options *options)
  :V1724(log, options){
	  DataFormatDefinition["channel_header_words"] = 0;
	  // MV boards seem to have reg 0x1n80 for channel n threshold
	  fChTrigRegister = 0x1080;
}

V1724_MV::~V1724_MV(){}
