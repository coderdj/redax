#include "CControl_Handler.hh"
#include "DAXHelpers.hh"
#include "MongoLog.hh"
#include "V2718.hh"
#include "f2718.hh"
#ifdef HASDDC10
#include "DDC10.hh"
#endif
#include "V1495.hh"
#include "V1495_tpc.hh"
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
    fCurrentRun = opts->GetInt("number", -1);
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
  std::vector<BoardType> bv = fOptions->GetBoards("V27XX");
  if(bv.size() != 1){
    fLog->Entry(MongoLog::Message, "Require one V2718 to be defined");
    fStatus = DAXHelpers::Idle;
    return -1;
  }
  BoardType cc = bv[0];
  try{
    if (cc.type == "f2718")
      fV2718 = std::make_unique<f2718>(fLog, copts, cc.link, cc.crate);
    else
      fV2718 = std::make_unique<V2718>(fLog, copts, cc.link, cc.crate);
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

  std::vector<BoardType> boards = fOptions->GetBoards("V1495");
  if (boards.size() == 1){
    BoardType v1495 = boards[0];
    fBID = v1495.board;
    if (v1495.type == "V1495_TPC")
      fV1495 = std::make_unique<V1495_TPC>(fLog, fOptions, fBID, fBoardHandle, v1495.vme_address);
    else
      fV1495 = std::make_unique<V1495>(fLog, fOptions, fBID, fBoardHandle, v1495.vme_address);

    std::map<std::string, int> opts;
    if (fOptions->GetV1495Opts(opts) < 0) {
      fLog->Entry(MongoLog::Warning, "Error getting V1495 options");
    } else if (fV1495->Arm(opts)) {
      fLog->Entry(MongoLog::Warning, "Could not initialize V1495");
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
  if(fV1495 && fV1495->BeforeSINStart()) {
    fLog->Entry(MongoLog::Error, "Could not start V1495");
    fStatus = DAXHelpers::Error;
    return -1;
  }

  if(!fV2718 || fV2718->SendStartSignal()!=0){
    fLog->Entry(MongoLog::Error, "V2718 either failed to start");
    fStatus = DAXHelpers::Error;
    return -1;
  }

  if(fV1495 && fV1495->AfterSINStart()) {
    fLog->Entry(MongoLog::Error, "Could not start V1495");
    fStatus = DAXHelpers::Error;
    return -1;
  }

  fStatus = DAXHelpers::Running;
  return 0;
}

// Stopping the previously started devices; V2718, V1495, DDC10...
int CControl_Handler::Stop(){
  if(fV2718){
    if (fV1495 && fV1495->BeforeSINStop()) {
      fLog->Entry(MongoLog::Warning, "Could not stop V1495");
    }
    if(fV2718->SendStopSignal() != 0){
      fLog->Entry(MongoLog::Warning, "Failed to stop V2718");
    }
    if (fV1495 && fV1495->AfterSINStop()) {
      fLog->Entry(MongoLog::Warning, "Could not stop V1495");
    }
  }
  fV1495.reset();
  fV2718.reset();
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
  using namespace bsoncxx::builder::stream;
  document builder{};
  builder << "host" << fHostname << "status" << fStatus <<
    "time" << bsoncxx::types::b_date(std::chrono::system_clock::now()) <<
    "mode" << (fOptions ? fOptions->GetString("name", "none") : "none") <<
    "number" << (fOptions ? fOptions->GetInt("number", -1) : -1);
  auto in_array = builder << "active" << open_array;

  if(fV2718){
    auto crate_options = fV2718->GetCrateOptions();
    in_array << open_document
         << "type" << "V2718"
	     << "s_in" << crate_options.s_in
	     << "neutron_veto" << crate_options.neutron_veto
	     << "muon_veto" << crate_options.muon_veto
	     << "led_trigger" << crate_options.led_trigger
	     << "pulser_freq" << crate_options.pulser_freq
	     << close_document;
  }
  auto after_array = in_array << close_array;
  auto doc = after_array << finalize;
  collection->insert_one(std::move(doc));
  return;
}
