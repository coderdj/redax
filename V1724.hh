#ifndef _V1724_HH_
#define _V1724_HH_

#include <unistd.h>
#include <cstring>

#include <stdint.h>
#include <CAENVMElib.h>
#include <vector>

#include <iostream>
#include "MongoLog.hh"
#include "Options.hh"

using namespace std;

class V1724{

 public:
  V1724(MongoLog *log);
  ~V1724();

  int Init(Options *options, int link, int crate, int bid, unsigned int address);
  int WriteRegister(unsigned int reg, unsigned int value);
  unsigned int ReadRegister(unsigned int reg);
  u_int32_t ReadMBLT(u_int32_t *&buffer);
  int ConfigureBaselines(vector <u_int16_t> &end_values,
			 int nominal_value=16000,
			 int ntries=100);
  int GetClockCounter(u_int32_t timestamp);
  int End();

  int bid(){
    return fBID;
  };

  int LoadDAC(vector<u_int16_t>dac_values, vector<bool> &update_dac);
  bool MonitorRegister(u_int32_t reg, u_int32_t mask, int ntries,
                       int sleep, u_int32_t val=1);

 private:
  void DetermineDataFormat(u_int32_t *buff, u_int32_t event_size,
			   u_int16_t channels_in_event);

  Options *fOptions;
  int fBoardHandle;
  int fLink, fCrate, fBID;
  unsigned int fBaseAddress;
  int fFirmwareVersion;

  // Stuff for clock reset tracking
  u_int32_t clock_counter;
  u_int32_t last_time;
  bool seen_under_5;
  bool seen_over_15;

  MongoLog *fLog;

};


#endif
