#include "CControl_Handler.hh"
#include "DAXHelpers.hh"


CControl_Handler::CControl_Handler(MongoLog *log, std::string procname){
  fOptions = NULL;
  fLog = log;
  fProcname = procname;
  fCurrentRun = -1;
  fV2718 = NULL;
  fDDC10 = NULL;
  fStatus = DAXHelpers::Idle;
}

CControl_Handler::~CControl_Handler(){
  DeviceStop();
}

// Initialising various devices namely; V2718 crate controller, DDC10...
int CControl_Handler::DeviceArm(int run, Options *opts){

  fStatus = DAXHelpers::Arming;
  
  // Just in case clear out any remaining objects from previous runs
  DeviceStop();

  fCurrentRun = run;  
  fOptions = opts;

  // Pull options for modules
  CrateOptions copts;
  if(fOptions->GetCrateOpt(copts, "V2718") != 0){
    fLog->Entry(MongoLog::Error,
		"Failed to pull crate options from file. Required fields: s_in, pulser_freq, muon_veto, neutron_veto, led_trigger");
    fStatus = DAXHelpers::Idle;
    return -1;
  }
  
  // Getting the link and crate for V2718
  std::vector<BoardType> bv = fOptions->GetBoards("V2718", fProcname);
  if(bv.size() != 1){
    fLog->Entry(MongoLog::Error, "Require one V2718 to be defined or we can't start the run");
    fStatus = DAXHelpers::Idle;
    return -1;
  }
  BoardType cc_def = bv[0];
    
  fV2718 = new V2718(fLog);
  
  if (fV2718->CrateInit(copts, cc_def.link, cc_def.crate, cc_def.vme_address)!=0){
    fLog->Entry(MongoLog::Error, "Failed to initialize V2718 crate controller");
    fStatus = DAXHelpers::Idle;
    return -1;
  }
  fStatus = DAXHelpers::Armed;
  return 0;
}
	   
// Send the start signal from crate controller
int CControl_Handler::DeviceStart(){
  if(fStatus != DAXHelpers::Armed){
    fLog->Entry(MongoLog::Warning, "V2718 attempt to start without arming. Maybe unclean shutdown");
    return 0;
  }
  if(fV2718 == NULL || fV2718->SendStartSignal()!=0){   
    fLog->Entry(MongoLog::Error, "V2718 either failed to start");
    fStatus = DAXHelpers::Error;
    return -1;
  }
  std::cout << "V2718 Started" << std::endl;
  fStatus = DAXHelpers::Running;
  return 0;
}

// Stopping the previously started devices; V2718, DDC10...
int CControl_Handler::DeviceStop(){

  // If V2718 here then send stop signal
  if(fV2718 != NULL){
    if(fV2718->SendStopSignal() != 0){
      fLog->Entry(MongoLog::Warning, "Failed to stop V2718");
    }
    delete fV2718;
    fV2718 = NULL;
  }

  /*
  if(fDDC10 != NULL){
    delete fDDC10;
    fDDC10 = NULL;
  }
  */
  fStatus = DAXHelpers::Idle;
  return 0;
}


// Reporting back on the status of V2718, DDC10 etc...
bsoncxx::document::value CControl_Handler::GetStatusDoc(std::string hostname){
 
  // Updating the status doc
  bsoncxx::builder::stream::document builder{};
  builder << "host" << hostname << "type" << "ccontrol" << "status" << fStatus;
  auto in_array = builder << "active" << bsoncxx::builder::stream::open_array;

  if(fV2718 != NULL){
    in_array << bsoncxx::builder::stream::open_document
	     << "run_number" << fCurrentRun
             << "type" << "V2718"
	     << "s_in" << fV2718->GetCrateOptions().s_in
	     << "neutron_veto" << fV2718->GetCrateOptions().neutron_veto
	     << "muon_veto" << fV2718->GetCrateOptions().muon_veto
	     << "led_trigger" << fV2718->GetCrateOptions().led_trigger
	     << "pulser_freq" << fV2718->GetCrateOptions().pulser_freq
	     << bsoncxx::builder::stream::close_document;
  }
  
  // Here you would add the DDC10
  
  auto after_array = in_array << bsoncxx::builder::stream::close_array;
  return after_array << bsoncxx::builder::stream::finalize;

}
     




