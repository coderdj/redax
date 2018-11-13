#include "CControl_Handler.hh"
#include "V2718.hh"


CControl_Handler::CControl_Handler(MongoLog *log){
  fOptions = NULL;
  fLog=log;
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
  else if(command == "send_stop_signal")
    std::cout<<"I would send a stop signal now"<<std::endl;
  
  return;
}



int CControl_Handler::CrateStart(std::string opts){ 
      std::cout<<"Initialising crate"<<std::endl;
      if(fOptions != NULL)
	      delete fOptions;
      fOptions = new Options(opts);
           
      //Getting the V2718 values from the options document
      for(auto c : fOptions->GetCrateOpt("V2718")){
               //std::cout << c_val.led_trig << std::endl;
	       
	       V2718 *v2718 = new V2718(fLog);
               if (v2718->CrateInit(c.led_trig, c.m_veto,c.n_veto,c.pulser_freq,c.s_in)==0){ 
	            std::cout << "Started Crate" << std::endl;
               }
               else{
                    std::cout << "Failed to Start Crate" << std::endl;
                    return -1;
              }
         }//end for
  return 0;
}
	       


//bsoncxx::document::value CControl_Handler::GetStatusDoc(std::string hostname){

  // Mind the types and be thankful for the python dictionary
  //bsoncxx::builder::stream::document builder{};


  //auto in_array = builder << "active" << bsoncxx::builder::stream::open_array;
  //for( auto const& el : fActiveRuns ){
  //  auto a = in_array << bsoncxx::builder::stream::open_document << "run" << el.first;
    // Status from V2718, V1495, DDC10
   // in_array = a << bsoncxx::builder::stream::close_document;
  //}
  //auto after_array = in_array << bsoncxx::builder::stream::close_array;
  //return after_array << bsoncxx::builder::stream::finalize;

//}
