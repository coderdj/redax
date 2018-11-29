#include "V2718.hh"

V2718::V2718(MongoLog *log){
  
  fBoardHandle=fLink=fCrate=-1; 
  fCopts.s_in = fCopts.neutron_veto = fCopts.muon_veto = -1;
  fCopts.led_trigger = fCopts.pulser_freq = -1;
  bStarted = false;
}


V2718::~V2718(){
}


int V2718::CrateInit(CrateOptions c_opts, int link, int crate){
        
  fCrate = crate;
  fLink = link;  	

  // Initialising the V2718 module via the specified optical link
  int a = CAENVME_Init(cvV2718, fLink, fCrate, &fBoardHandle);
  if(a != cvSuccess){
    fLog->Entry("Failed to init V2718 with CAEN error: " + str(a), MongoLog::Error);
    return -1;
  }   
  return 0;
}


int V2718::SendStartSignal(){

  // Straight copy from: https://github.com/coderdj/kodiaq

  // Line 0 : S-IN. 
  CAENVME_SetOutputConf(fCrate, cvOutput0, cvDirect, 
			cvActiveHigh, cvManualSW);
  // Line 1 : MV S-IN Logic
  CAENVME_SetOutputConf(fCrate, cvOutput1, cvDirect, 
			cvActiveHigh, cvManualSW);
  // Line 2 : LED Logic
  CAENVME_SetOutputConf(fCrate, cvOutput2, cvDirect, 
			cvActiveHigh, cvManualSW);
  // Line 3 : LED Pulser
  CAENVME_SetOutputConf(fCrate, cvOutput3, cvDirect, 
			cvActiveHigh, cvMiscSignals);
  // Line 4 : NV S-IN Logic   
  CAENVME_SetOutputConf(fCrate, cvOutput4, cvDirect,
                        cvActiveHigh, cvMiscSignals); // soonTM


  // Set the output register
  unsigned int data = 0x0;
  if(fCopts.neutron_veto)            //n_veto soonTM
    data+=cvOut4Bit;       
  if(fCopts.led_trigger)
    data+=cvOut2Bit;
  if(fCopts.muon_veto)
    data+=cvOut1Bit;
  if(fCopts.s_in)
    data+=cvOut0Bit;

  // S-IN and logic signals 
  if(CAENVME_SetOutputRegister(fCrate,data)!=0){
    fLog->Entry("Couldn't set output register to crate controller", MongoLog::Error);
    return -1;
  }
 
  //Configure the LED pulser
  if(fOpts.pulser_freq > 0){
    // We allow a range from 1Hz to 1MHz, but this is not continuous!
    // If the number falls between two possible ranges it will be rounded to 
    // the maximum value of the lower one
    CVTimeUnits tu = cvUnit104ms;
    u_int32_t width = 0x1;
    u_int32_t period = 0x0; 
      
     if(fOpts.pulser_freq < 10){
	if(i_pulser_Hz > 5)
	   period = 0xFF;
	else
	   period = (u_int32_t)((1000/104) / fOpts.pulser_freq);
      }
     else if(fOpts.pulser_freq < 2450){
	tu = cvUnit410us;
	if(fOpts.pulser_freq > 1219)
	   period = 0xFF;
	else
	   period = (u_int32_t)((1000000/410) / fOpts.pulser_freq);
      }
      else if(fOpts.pulser_freq < 312500){
	tu = cvUnit1600ns;
	period = (u_int32_t)((1000000/1.6) / fOpts.pulser_freq);
      }
      else if(fOpts.pulser_freq < 20000000){
	tu = cvUnit25ns;
        period = (u_int32_t)((1E9/25) / fOpts.pulser_freq);
      }
      else{
         std::stringstream error;
         error<< "Given an invalid LED frequency!";
         fLog->Entry(error.str(), MongoLog::Error);
      }
    // Set pulser    
    int ret = CAENVME_SetPulserConf(fCrate, cvPulserB, period, width, tu, 0,
	 		  cvManualSW, cvManualSW);
    ret *= CAENVME_StartPulser(fCrate,cvPulserB); 
    if(ret != cvSuccess){
      fLog->Entry("Failed to activate LED pulser", MongoLog::Warning);
    }
  }
  return 0;
}


int V2718::SendStopSignal(){

  // Stop the pulser if it's running
  CAENVME_StopPulser(fCrate, cvPulserB);
  usleep(1000);
 
  // Line 0 : S-IN.
  CAENVME_SetOutputConf(fCrate, cvOutput0, cvDirect, cvActiveHigh, cvManualSW);
  // Line 1 : MV S-IN Logic 
  CAENVME_SetOutputConf(fCrate, cvOutput1, cvDirect, cvActiveHigh, cvManualSW);
  // Line 2 : LED Logic
  CAENVME_SetOutputConf(fCrate, cvOutput2, cvDirect, cvActiveHigh, cvManualSW);
  // Line 3 : LED Pulser
  CAENVME_SetOutputConf(fCrate, cvOutput3, cvDirect, cvActiveHigh, cvMiscSignals);
  // Line 4 : NV S-IN Logic
  CAENVME_SetOutputConf(fCrate, cvOutput4, cvDirect, cvActiveHigh, cvManualSW);
  

  // Set the output register 
  unsigned int data = 0x0; 
  CAENVME_SetOutputRegister(fCrate,data);
  if(CAENVME_End(fCrate)!= cvSuccess){
     std::cout << "Failed to end crate" << std::endl;  
  }
   fBoardHandle=fLink=fCrate=-1; 
  return 0;   
}

