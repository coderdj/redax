#include "CControl_Handler.hh"
#include "DAXHelpers.hh"
#include "V2718.hh"


CControl_Handler::CControl_Handler(MongoLog *log){
  fOptions = NULL;
  fLog = log;
}

CControl_Handler::~CControl_Handler(){	
  if(fOptions != NULL)
      delete fOptions;
}

void CControl_Handler::ProcessCommand(std::string command, std::string detector,
				      int run, std::string options){
  std::cout<<"Found commmand "<<command<<" for detector "<<detector<<" and run "<<
    run<<" with mode: "<<options<<std::endl;
  if(command == "start")
    std::cout<<"I would send a start signal now"<<std::endl;
  else if(command == "stop")
    std::cout<<"I would send a stop signal now"<<std::endl;
  else if(command == "arm")
    std::cout<<"I would arm the DAQ now"<<std::endl;

  return;
}

// Initialising various devices namely; V2718 crate controller, V1495, DDC10...
int CControl_Handler::DeviceArm(int run, std::string opts){
      if(fOptions != NULL)
	      delete fOptions;
      fOptions = new Options(opts);
      
      // Getting the link and crate for V2718
      for(auto o : fOptions->GetBoards("V2718","")){
          //Getting the V2718 settings from the options document
          for(auto c : fOptions->GetCrateOpt("V2718")){	
	    modules m_devs;	    
	    m_devs.v2718 = new V2718(fLog);
	    m_devs.number = run;
            
	    fActiveRuns[0] = m_devs; 	    // Don't really need a map only a struct would be fine imho?!?
            //m_devs.v1495 = new V1495(fLog);  //soonTM
	    //m_devs.DDC10 = new DDC10(fLOg);  //soonTM

             if (fActiveRuns[0].v2718->CrateInit(c.led_trig, c.m_veto, 
				      c.n_veto, c.pulser_freq, c.s_in, o.link, o.crate)==0){	       
             }else{
                 std::cout << "Failed to initialise V2718" << std::endl;
                 return -1;
             }
          }//end GetCrateOpt for
       }// end GetBoards for
  return 0;
}
	   
// Starting the armed devices
int CControl_Handler::DeviceStart(){      
    for(auto const& el : fActiveRuns){
      if (el.second.v2718->SendStartSignal()==0){
         std::cout << "V2718 Started" << std::endl;
      }else{
         std::cout << "Failed to start V2718" << std::endl;
	 return -1;
      }
     }
  return 0;
}

// Stopping the previously started devices; V2718, V1495, DDC10...
int CControl_Handler::DeviceStop(){
    for(auto const& el : fActiveRuns){
      if (el.second.v2718->SendStopSignal()==0){
         std::cout << "V2718 Stopped" << std::endl;
      }else{
         std::cout << "Failed to stop V2718" << std::endl;
      }
    }
  fActiveRuns = {}; //reset struct
  return 0;
}


// Reporting back on the status of V2718, V1495, DDC10 etc...
bsoncxx::document::value CControl_Handler::GetStatusDoc(std::string hostname){

  std::string ch_time = DAXHelpers::GetChronoTimeString();	
  // Updating the status doc
  bsoncxx::builder::stream::document builder{};
  builder << "host" << hostname << "type" << "ccontrol" << "date" << ch_time;
  auto in_array = builder << "active" << bsoncxx::builder::stream::open_array;
  
  if(fOptions != NULL){
     for(auto c : fOptions->GetCrateOpt("V2718")){
         for(auto const& el : fActiveRuns ){
             auto a = in_array << bsoncxx::builder::stream::open_document 
             << "run number" << el.second.number << "V2718 :" 
	     <<  bsoncxx::builder::stream::open_document 
	     << "s_in" << c.s_in << "n_veto" << c.n_veto 
	     << "m_veto" << c.m_veto << "pulser_freq" << c.pulser_freq
	      << bsoncxx::builder::stream::close_document;
             // + status from V1495, DDC10...etc
             in_array = a << bsoncxx::builder::stream::close_document;
	 }
     }
  }
  auto after_array = in_array << bsoncxx::builder::stream::close_array;
  return after_array << bsoncxx::builder::stream::finalize;
}
     




