#include "CControl_Handler.hh"

CControl_Handler::CControl_Handler(){
}

CControl_Handler::~CControl_Handler(){
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

bsoncxx::document::value CControl_Handler::GetStatusDoc(std::string hostname){
  /*
    Base form
    {
      host: hostname
      date: now
      type: ccontrol
      active: [
        {
            run: number
            V2718: { sin: 0/1, mveto: 0/1, nveto: 0/1, pulser: hz } // optional
            V1495: { some update } // optional
            DDC10: { some update } // optional
        }
      ]
   */

  // Mind the types and be thankful for the python dictionary
  bsoncxx::builder::stream::document builder{};

  // Top level fields
  builder << "host" << hostname << "type" << "ccontrol";

  auto in_array = builder << "active" << bsoncxx::builder::stream::open_array;
  for( auto const& el : fActiveRuns ){
    auto a = in_array << bsoncxx::builder::stream::open_document << "run" << el.first;
    // Status from V2718, V1495, DDC10
    in_array = a << bsoncxx::builder::stream::close_document;
  }
  auto after_array = in_array << bsoncxx::builder::stream::close_array;

  return after_array << bsoncxx::builder::stream::finalize;
}
