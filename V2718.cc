#include "V2718.hh"

V2718::V2718(MongoLog *log){
  
  b_startwithsin = false;
  b_mveto_on = false;
  b_nveto_on = false;
  b_led_on = false;
  i_pulser_Hz = 0;
  bStarted = false;

  fBaseAddress = 0;
  fLink=1;
  fCrate = fBoardHandle = -1;
  fLog = log;

}


V2718::~V2718(){
};



int V2718::CrateInit(int led_trig, int m_veto, int n_veto, int pulser_freq, int s_in){
       
	  b_startwithsin = false;
          b_mveto_on = false;
          b_nveto_on = false;
          b_led_on = false;
          i_pulser_Hz = 0;
          bStarted = false;

	std::cout << "Getting crate values" << std::endl;
   
	    if(n_veto == 1){
               b_nveto_on = true;
            }
            if(m_veto == 1){
                b_mveto_on = true;
            } 
            if(s_in == 1){
                b_startwithsin = true;
            }
            if(pulser_freq > 0){
                i_pulser_Hz = pulser_freq;
            }
            if(led_trig == 1 ){
                b_led_on = true;
            }
            else{
               	  std::stringstream error;
                  error<< "Could not get crate values";
		  fLog->Entry(error.str(), MongoLog::Error);
            }
     return 0;
}


int V2718::SendStartSignal(){
  // This is just a straight copy from XENON1T KODIAQ
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


  // Set the output register
  unsigned int data = 0x0;
  if(b_led_on)
    data+=cvOut2Bit;          // Output line 2 signal level
  if(b_mveto_on)            
    data+=cvOut1Bit;          // Output line 1 signal level
  //if(b_nveto_on)
  //  data+=cvOut4Bit;          // Output line 4 signal level
  if(b_startwithsin)
    data+=cvOut0Bit;          // Output line 0 signal level 


   // This is the S-IN
  if(CAENVME_SetOutputRegister(fCrate,data)!=0){
	          std::stringstream error;
                  error<< "Could't set output register. Crate Controller not found";
                  fLog->Entry(error.str(), MongoLog::Error);
  return -1;
  }
  
  bStarted=true;  
  //Configure the LED pulser
  if(i_pulser_Hz > 0){
    // We allow a range from 1Hz to 1MHz, but this is not continuous!
    // If the number falls between two possible ranges it will be rounded to 
    // the maximum value of the lower one
    CVTimeUnits tu = cvUnit104ms;
    u_int32_t width = 0x1;
    u_int32_t period = 0x0; 
      
     if(i_pulser_Hz < 10){
	if(i_pulser_Hz > 5)
	   period = 0xFF;
	else
	   period = (u_int32_t)((1000/104) / i_pulser_Hz);
      }
     else if(i_pulser_Hz < 2450){
	tu = cvUnit410us;
	if(i_pulser_Hz >1219)
	   period = 0xFF;
	else
	   period = (u_int32_t)((1000000/410) / i_pulser_Hz);
      }
      else if(i_pulser_Hz < 312500){
	tu = cvUnit1600ns;
	period = (u_int32_t)((1000000/1.6) / i_pulser_Hz);
      }
      else if(i_pulser_Hz < 20000000){
	tu = cvUnit25ns;
        period = (u_int32_t)((1E9/25)/i_pulser_Hz);
      }
      else{
         std::stringstream error;
         error<< "Given an invalid LED frequency!";
         fLog->Entry(error.str(), MongoLog::Error);
      }
   // m_koLog->Message("Writing period with: "+koHelper::IntToString(period)+
  //		     " and width with " +koHelper::IntToString(width));
    // Send data to the board
    int ret = CAENVME_SetPulserConf(fCrate, cvPulserB, period, width, tu, 0,
			  cvManualSW, cvManualSW);
    if(ret!=cvSuccess ){
	    std::cout << "we have a problem !" << std::endl;    
    }

    CAENVME_StartPulser(fCrate,cvPulserB);    
  }
  return 0;
}


int V2718::SendStopSignal()
{
  bStarted = false;
  // Stop the pulser if it's running
  CAENVME_StopPulser(fCrate, cvPulserB);
  usleep(1000);
  //u_int16_t data = 0x7C8;
  //u_int16_t data = 0x7FF;
  //cout<<"Writing cvOutRegClear with: "<<data<<endl;

  // Line 0 : S-IN.
  CAENVME_SetOutputConf(fCrate, cvOutput0, cvDirect, cvActiveHigh, cvManualSW);
  // Line 1 : MV S-IN Logic 
  CAENVME_SetOutputConf(fCrate, cvOutput1, cvDirect, cvActiveHigh, cvManualSW);
  // Line 2 : LED Logic
  CAENVME_SetOutputConf(fCrate, cvOutput2, cvDirect, cvActiveHigh, cvManualSW);
  // Line 3 : LED Pulser
  CAENVME_SetOutputConf(fCrate, cvOutput3, cvDirect, cvActiveHigh, cvMiscSignals);

  // Set the output register                                                                                                                       
  unsigned int data = 0x0;
  
  CAENVME_SetOutputRegister(fCrate,data);

  return 0;   
}
























