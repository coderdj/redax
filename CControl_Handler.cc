#include "CControl_Handler.hh"
#include "DAXHelpers.hh"
#include "V2718.hh"
#ifdef HASDDC10
#include "DDC10.hh"
#endif
#include "V1495.hh"
#include <bsoncxx/builder/stream/document.hpp>

CControl_Handler::CControl_Handler(std::shared_ptr<MongoLog>& log, std::string procname) : DAQController(log, procname){
  fCurrentRun = fBID = fBoardHandle = -1;
  fV2718 = nullptr;
  fV1495 = nullptr;
#ifdef HASDDC10
  fDDC10 = nullptr;
#endif
  fStatus = DAXHelpers::Idle;
}

CControl_Handler::~CControl_Handler(){
  Stop();
}

// Initialising various devices namely; V2718 crate controller, V1495, DDC10...
int CControl_Handler::Arm(std::shared_ptr<Options>& opts){

  fStatus = DAXHelpers::Arming;

  // Just in case clear out any remaining objects from previous runs
  Stop();

  fOptions = opts;
  try{
    fCurrentRun = opts->GetInt("run_identifier", -1);
  }catch(std::exception& e) {
    fLog->Entry(MongoLog::Warning, "No run number specified in config?? %s", e.what());
    return -1;
  }

  // Pull options for V2718
  CrateOptions copts;
  if(fOptions->GetCrateOpt(copts) != 0){
    fLog->Entry(MongoLog::Error,
		"Failed to pull crate options from file. Required fields: s_in, pulser_freq, muon_veto, neutron_veto, led_trigger");
    fStatus = DAXHelpers::Idle;
    return -1;
  }

  // Getting the link and crate for V2718
  std::vector<BoardType> bv = fOptions->GetBoards("V2718");
  if(bv.size() != 1){
    fLog->Entry(MongoLog::Message, "Require one V2718 to be defined");
    fStatus = DAXHelpers::Idle;
    return -1;
  }
  BoardType cc_def = bv[0];
  try{
    fV2718 = std::make_unique<V2718>(fLog, copts, cc_def.link, cc_def.crate);
  }catch(std::exception& e){
    fLog->Entry(MongoLog::Error, "Failed to initialize V2718 crate controller: %s", e.what());
    fStatus = DAXHelpers::Idle;
    return -1;
  }
  fBoardHandle = fV2718->GetHandle();
  fLog->Entry(MongoLog::Local, "V2718 Initialized");

#ifdef HASDDC10
  // Getting options for DDC10 HEV module
  std::vector<BoardType> dv = fOptions->GetBoards("DDC10");
  if (dv.size() == 1){
    HEVOptions hopts;
     if(fOptions->GetHEVOpt(hopts) == 0){
        fDDC10 = std::make_unique<DDC10>();
	if(fDDC10->Initialize(hopts) != 0){
           fLog->Entry(MongoLog::Error, "Failed to initialise DDC10 HEV");
	   fStatus = DAXHelpers::Idle;
	   return -1;
	}else{
	   fLog->Entry(MongoLog::Local, "DDC10 Initialised");
	}
     }else{
	fLog->Entry(MongoLog::Error, "Failed to pull DDC10 options from file");
     }
  } else {
  }
#endif // HASDDC10

  std::vector<BoardType> mv = fOptions->GetBoards("V1495");
  if (mv.size() == 1){
    BoardType mv_def = mv[0];
    fBID = mv_def.board;
    fV1495 = std::make_unique<V1495>(fLog, fOptions, mv_def.board, fBoardHandle, mv_def.vme_address);
	// Writing registers to the V1495 board
	for(auto regi : fOptions->GetRegisters(fBID, true)){
		unsigned int reg = DAXHelpers::StringToHex(regi.reg);
		unsigned int val = DAXHelpers::StringToHex(regi.val);
		if(fV1495->WriteReg(reg, val)!=0){
			fLog->Entry(MongoLog::Error, "Failed to initialise V1495 board");
			fStatus = DAXHelpers::Idle;
			return -1;
		}
    }
  }else{
  }
  fStatus = DAXHelpers::Armed;
  return 0;

}

int CControl_Handler::Start(){
  if(fStatus != DAXHelpers::Armed){
    fLog->Entry(MongoLog::Warning, "V2718 attempt to start without arming. Maybe unclean shutdown");
    return 0;
  }
  if(!fV2718 || fV2718->SendStartSignal()!=0){
    fLog->Entry(MongoLog::Error, "V2718 either failed to start");
    fStatus = DAXHelpers::Error;
    return -1;
  }

  fStatus = DAXHelpers::Running;
  return 0;
}

// Stopping the previously started devices; V2718, V1495, DDC10...
int CControl_Handler::Stop(){
  if(fV2718){
    if(fV2718->SendStopSignal() != 0){
      fLog->Entry(MongoLog::Warning, "Failed to stop V2718");
    }
    fV2718.reset();
  }
  fV1495.reset();
#ifdef HASDDC10
  // Don't need to stop the DDC10 but just clean up a bit
  fDDC10.reset();
#endif
  fOptions.reset();

  fStatus = DAXHelpers::Idle;
  return 0;
}

// Reporting back on the status of V2718, V1495, DDC10 etc...
void CControl_Handler::StatusUpdate(mongocxx::collection* collection){
  bsoncxx::builder::stream::document builder{};
  builder << "host" << fHostname << "status" << fStatus <<
    "time" << bsoncxx::types::b_date(std::chrono::system_clock::now()) <<
    "mode" << (fOptions ? fOptions->GetString("name", "none") : "none") <<
    "number" << (fOptions ? fOptions->GetInt("number", -1) : -1);
  auto in_array = builder << "active" << bsoncxx::builder::stream::open_array;

  if(fV2718){
    auto crate_options = fV2718->GetCrateOptions();
    in_array << bsoncxx::builder::stream::open_document
         << "type" << "V2718"
	     << "s_in" << crate_options.s_in
	     << "neutron_veto" << crate_options.neutron_veto
	     << "muon_veto" << crate_options.muon_veto
	     << "led_trigger" << crate_options.led_trigger
	     << "pulser_freq" << crate_options.pulser_freq
	     << bsoncxx::builder::stream::close_document;
  }
  auto after_array = in_array << bsoncxx::builder::stream::close_array;
  collection->insert_one(after_array << bsoncxx::builder::stream::finalize);
  return;
  /*
  // DDC10 parameters might change for future updates of the XENONnT HEV
  if(fDDC10){
    auto hev_options = fDDC10->GetHEVOptions();
    in_array << bsoncxx::builder::stream::open_document
             << "type" << "DDC10"
	     << "Address" << hev_options.address
             << "required" << hev_options.required
             << "signal_threshold" << hev_options.signal_threshold
             << "sign" << hev_options.sign
             << "rise_time_cut" << hev_options.rise_time_cut
             << "inner_ring_factor" << hev_options.inner_ring_factor
             << "outer_ring_factor" << hev_options.outer_ring_factor
             << "integration_threshold" << hev_options.integration_threshold
             << "parameter_0" << hev_options.parameter_0
             << "parameter_1" << hev_options.parameter_1
             << "parameter_2" << hev_options.parameter_2
             << "parameter_3" << hev_options.parameter_3
             << "window" << hev_options.window
             << "prescaling" << hev_options.prescaling
             << "component_status" << hev_options.component_status
             << "width_cut" << hev_options.width_cut
             << "delay" << hev_options.delay
             << bsoncxx::builder::stream::close_document;
  }
  // Write the settings for the Muon Veto V1495 board into status doc 
  if(fV1495){
    auto registers = fOptions->GetRegisters(fBID);
     in_array << bsoncxx::builder::stream::open_document
	      << "type" << "V1495"
	      << "Module reset" << registers[0].val
	      << "Mask A" << registers[1].val
	      << "Mask B" << registers[2].val
	      << "Mask D" << registers[3].val
	      << "Majority Threshold" << registers[4].val
	      << "Coincidence Window" << registers[5].val
	      << "NIM/TTL CTRL" << registers[6].val
	      << bsoncxx::builder::stream::close_document; 
  }
  
  after_array = in_array << bsoncxx::builder::stream::close_array;
  return after_array << bsoncxx::builder::stream::finalize;
*/
}
