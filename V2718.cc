#include "V2718.hh"
#include "MongoLog.hh"
#include <CAENVMElib.h>

V2718::V2718(std::shared_ptr<MongoLog>& log, CrateOptions c_opts){
  fLog = log;
  fBoardHandle=-1;

  fCopts = c_opts;
}

V2718::~V2718(){
}

int V2718::Init(int link, int crate) {
  if (CAENVME_Init(cvV2718, link, crate, &fBoardHandle))
    return -1;
  return SendStopSignal(false);
}

int V2718::SendStartSignal(){

  // Straight copy from: https://github.com/coderdj/kodiaq

  // Line 0 : S-IN.
  CAENVME_SetOutputConf(fCrate, cvOutput0, cvDirect, cvActiveHigh, cvManualSW);
  // Line 1 : MV S-IN Logic
  CAENVME_SetOutputConf(fCrate, cvOutput1, cvDirect, cvActiveHigh, cvManualSW);
  // Line 2 : LED Logic
  CAENVME_SetOutputConf(fCrate, cvOutput2, cvDirect, cvActiveHigh, cvManualSW);
  // Line 3 : LED Pulser
  CAENVME_SetOutputConf(fCrate, cvOutput3, cvDirect, cvActiveHigh, cvMiscSignals);
  // Line 4 : NV S-IN Logic
  CAENVME_SetOutputConf(fCrate, cvOutput4, cvDirect, cvActiveHigh, cvMiscSignals); // soonTM


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
    fLog->Entry(MongoLog::Error, "Couldn't set output register to crate controller");
    return -1;
  }

  //Configure the LED pulser
  if(fCopts.pulser_freq > 0){
    //CAEN supports frequencies from 38 mHz to 40 MHz, but it's not continuous
    //We tell the CC about the time unit (104 ms, 410 us, 1.6 us, 25ns)
    //and how many of them (1-FF) to set the period
    CVTimeUnits tu = cvUnit104ms;
    u_int32_t width = 0x1;
    u_int32_t period = 0x0;
    std::vector<CVTimeUnits> tus = {cvUnit104ms, cvUnit410us, cvUnit1600ns, cvUnit25ns};
    std::vector<double> widths = {104e-3, 410e-6, 1.6e-6, 25e-9};

    for (unsigned i = 0; i < widths.size(); i++) {
      if (fCopts.pulser_freq < 1./widths[i]) {
        period = std::clamp(int(1./(widths[i]*fCopts.pulser_freq)), 1, 0xFF);
        tu = tus[i];
        fLog->Entry(MongoLog::Debug, "Closest freq to %.1f Hz is %.1f",
              fCopts.pulser_freq, 1./(period*widths[i]));
        break;
      }
      if (i == 3) {
        fLog->Entry(MongoLog::Error, "Given an invalid LED frequency");
        return -1;
      }
    }
    // Set pulser
    int ret = CAENVME_SetPulserConf(fCrate, cvPulserB, period, width, tu, 0,
                                    cvManualSW, cvManualSW);
    ret *= CAENVME_StartPulser(fCrate,cvPulserB);
    if(ret != cvSuccess){
      fLog->Entry(MongoLog::Warning, "Failed to activate LED pulser");
      return -1;
    }
  }
  return 0;
}

int V2718::SendStopSignal(bool end){

  if(fBoardHandle == -1)
    return 0;

  // Stop the pulser if it's running
  CAENVME_StopPulser(fBoardHandle, cvPulserB);
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
  CAENVME_SetOutputRegister(fCrate, data);

  if(end){
    if(CAENVME_End(fBoardHandle)!= cvSuccess){
      fLog->Entry(MongoLog::Warning, "Failed to end crate");
    }
    fBoardHandle=-1;
  }
  return 0;
}

