#include "V1730.hh"
#include "MongoLog.hh"
#include "Options.hh"

V1730::V1730(MongoLog *log, Options *options)
  :V1724(log, options){
  fNChannels = 16;
  DataFormatDefinition["ns_per_sample"] = 2;
  DataFormatDefinition["ns_per_clk"] = 2;
  DataFormatDefinition["channel_header_words"] = 3;
  DataFormatDefinition["channel_mask_msb_idx"] = 2;
  DataFormatDefinition["channel_mask_msb_mask"] = -1;
  DataFormatDefinition["channel_time_msb_idx"] = 2; 
  DataFormatDefinition["channel_time_msb_mask"] = -1;
  // Channel indices are given relative to start of channel
  // i.e. the channel size is at index '0'
  
}

V1730::~V1730(){}
