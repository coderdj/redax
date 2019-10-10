#include "CControl_Handler.hh"
#include "DAXHelpers.hh"

CControl_Handler::CControl_Handler(MongoLog *log, std::string procname){
  fOptions = NULL;
  fLog = log;
  fProcname = procname;
  fCurrentRun = -1;
  fV2718 = NULL;
  fV1495 = NULL;
  fDDC10 = NULL;
  fStatus = DAXHelpers::Idle;
}

CControl_Handler::~CControl_Handler(){
  DeviceStop();
}

// Initialising various devices namely; V2718 crate controller, V1495, DDC10...
int CControl_Handler::DeviceArm(int run, Options *opts){

  fStatus = DAXHelpers::Arming;
  
  // Just in case clear out any remaining objects from previous runs
  DeviceStop();

  fCurrentRun = run;  
  fOptions = opts;

// ----------------------------------------------
  // Pull options for V2718
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
    fLog->Entry(MongoLog::Message, "Require one V2718 to be defined or we can't start the run");
    fStatus = DAXHelpers::Idle;
    return -1;
  }
  BoardType cc_def = bv[0];   
  fV2718 = new V2718(fLog);
  if (fV2718->CrateInit(copts, cc_def.link, cc_def.crate)!=0){
    fLog->Entry(MongoLog::Error, "Failed to initialize V2718 crate controller");
    fStatus = DAXHelpers::Idle;
    return -1;
  }else{
     std::cout << "V2718 Initialised" << std::endl;
  }

// ----------------------------------------------
  // Getting options for DDC10 HEV module
  HEVOptions hopts;
  fDDC10 = new DDC10();

  if(fOptions->GetHEVOpt(hopts, "DDC10") != 0){
     fLog->Entry("Failed to pull DDC10 options from file", MongoLog::Error);
     fStatus = DAXHelpers::Idle;
     return -1;
  }
  // Initialise the DDC10 based HEV - it doesn't need further "start"/"stop" signals
  if(fDDC10->Initialize(hopts)!=0){
    fLog->Entry("Failed to initialise DDC10 HEV", MongoLog::Error);
    fStatus = DAXHelpers::Idle;
    return -1;
  }else{
     std::cout << "DDC10 Initialised" << std::endl;
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

  fStatus = DAXHelpers::Running;
  return 0;
}

// Stopping the previously started devices; V2718, V1495, DDC10...
int CControl_Handler::DeviceStop(){

  // If V2718 here then send stop signal
  if(fV2718 != NULL){
    if(fV2718->SendStopSignal() != 0){
      fLog->Entry(MongoLog::Warning, "Failed to stop V2718");
    }
    delete fV2718;
    fV2718 = NULL;
  }
  // Don't need to stop the DDC10 but just clean up a bit
  if(fDDC10 != NULL){
    delete fDDC10;
    fDDC10 = NULL;
  }

  /*
  if(fV1495 != NULL){
    delete fV1495;
    fV1495 = NULL;
  }
  */
  fStatus = DAXHelpers::Idle;
  return 0;
}


// Reporting back on the status of V2718, V1495, DDC10 etc...
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
  // DDC10 parameters might change for future updates of the XENONnT HEV
  if(fDDC10 != NULL){
    in_array << bsoncxx::builder::stream::open_document
             << "Address" << fDDC10->GetHEVOptions().address
             << "required" << fDDC10->GetHEVOptions().required
             << "signal_threshold" << fDDC10->GetHEVOptions().signal_threshold
             << "sign" << fDDC10->GetHEVOptions().sign
             << "rise_time_cut" << fDDC10->GetHEVOptions().rise_time_cut
             << "inner_ring_factor" << fDDC10->GetHEVOptions().inner_ring_factor
             << "outer_ring_factor" << fDDC10->GetHEVOptions().outer_ring_factor
             << "integration_threshold" << fDDC10->GetHEVOptions().integration_threshold
             << "parameter_0" << fDDC10->GetHEVOptions().parameter_0
             << "parameter_1" << fDDC10->GetHEVOptions().parameter_1
             << "parameter_2" << fDDC10->GetHEVOptions().parameter_2
             << "parameter_3" << fDDC10->GetHEVOptions().parameter_3
             << "window" << fDDC10->GetHEVOptions().window
             << "prescaling" << fDDC10->GetHEVOptions().prescaling
             << "component_status" << fDDC10->GetHEVOptions().component_status
             << "width_cut" << fDDC10->GetHEVOptions().width_cut
             << "delay" << fDDC10->GetHEVOptions().delay
             << bsoncxx::builder::stream::close_document;
  }

  // Here you would add the V1495...
  
  auto after_array = in_array << bsoncxx::builder::stream::close_array;
  return after_array << bsoncxx::builder::stream::finalize;

}  




