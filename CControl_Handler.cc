#include "CControl_Handler.hh"
#include "DAXHelpers.hh"
#include "V2718.hh"


CControl_Handler::CControl_Handler(MongoLog *log, std::string procname){
  fOptions = NULL;
  fLog = log;
  fProcname = procname;
  fCurrentRun = -1;
  fV2718 = NULL;
  fV1495 = NULL;
  fDDC10 = NULL;
}

CControl_Handler::~CControl_Handler(){
  DeviceStop();
  if(fOptions != NULL)
      delete fOptions;
}

// Initialising various devices namely; V2718 crate controller, V1495, DDC10...
int CControl_Handler::DeviceArm(int run, std::string opts, std::vector<std::string>include_json,
				std::string override){

  // Just in case clear out any remaining objects from previous runs
  DeviceStop();

  fCurrentRun = run;
  
  // This will throw if, for example, you can't reach the opts DB or if you
  // can't find an options doc with the name you're trying to invoke
  try{
    fOptions = new Options(opts, include_json);
    if(override!=""){
    fOptions->Override(bsoncxx::from_json(override).view());
  }
    
  }catch(std::exception E){
    fLog->Entry("Exception when loading options: " + std::string(E.what()), MongoLog::Error);
    return -1;
  }

  // Pull options for modules
  CrateOptions copts;
  if(fOptions->GetCrateOpt(copts, "V2718") != 0){
    fLog->Entry("Failed to pull crate options from file. Required fields: s_in, pulser_freq, muon_veto, neutron_veto, led_trigger", MongoLog::Error);
    return -1;
  }
  
  // Getting the link and crate for V2718
  std::vector<BoardType> bv = fOptions->GetBoards("V2718", fProcname);
  if(bv.size() != 1){
    fLog->Entry("Require one V2718 to be defined or we can't start the run", MongoLog::Error);
    return -1;
  }
  BoardType cc_def = bv[0];
    
  fV2718 = new V2718(fLog);
  
  if (fV2718->CrateInit(copts, cc_def.link, cc_def.crate)!=0){
    fLog->Entry("Failed to initialize V2718 crate controller", MongoLog::Error);
    return -1;
  }

  return 0;
}
	   
// Send the start signal from crate controller
int CControl_Handler::DeviceStart(){      
  if(fV2718 == NULL || fV2718->SendStartSignal()!=0){   
    fLog->Entry("V2718 either undefined or failed to start", MongoLog::Error);
    return -1;
  }
  std::cout << "V2718 Started" << std::endl; 
  return 0;
}

// Stopping the previously started devices; V2718, V1495, DDC10...
int CControl_Handler::DeviceStop(){

  // If V2718 here then send stop signal
  if(fV2718 != NULL){
    if(fV2718->SendStopSignal() != 0){
      fLog->Entry("Failed to stop V2718", MongoLog::Warning);
    }
    delete fV2718;
    fV2718 = NULL;
  }

  /*
  if(fV1495 != NULL){
    delete fV1495;
    fV1495 = NULL;
  }
  if(fDDC10 != NULL){
    delete fDDC10;
    fDDC10 = NULL;
  }
  */
  if(fOptions != NULL){
    delete fOptions;
    fOptions = NULL;
  }
  return 0;
}


// Reporting back on the status of V2718, V1495, DDC10 etc...
bsoncxx::document::value CControl_Handler::GetStatusDoc(std::string hostname){
 
  // Updating the status doc
  bsoncxx::builder::stream::document builder{};
  builder << "host" << hostname << "type" << "ccontrol";
  auto in_array = builder << "active" << bsoncxx::builder::stream::open_array;

  if(fV2718 != NULL){
    in_array << bsoncxx::builder::stream::open_document
	     << "run_number" << fCurrentRun
	     << "s_in" << fV2718->GetCrateOptions().s_in
	     << "neutron_veto" << fV2718->GetCrateOptions().neutron_veto
	     << "muon_veto" << fV2718->GetCrateOptions().muon_veto
	     << "led_trigger" << fV2718->GetCrateOptions().led_trigger
	     << "pulser_freq" << fV2718->GetCrateOptions().pulser_freq
	     << bsoncxx::builder::stream::close_document;
  }
  
  // Here you would add the DDC10 and V1495
  
  auto after_array = in_array << bsoncxx::builder::stream::close_array;
  return after_array << bsoncxx::builder::stream::finalize;

}
     




