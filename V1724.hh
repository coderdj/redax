#ifndef _V1724_HH_
#define _V1724_HH_

#include <unistd.h>
#include <cstring>

#include <stdint.h>
#include <CAENVMElib.h>
#include <vector>

#include <iostream>
#include "MongoLog.hh"

using namespace std;

class V1724{

 public:
  V1724(MongoLog *log);
  ~V1724();

  int Init(int link, int crate, int bid, unsigned int address);
  int WriteRegister(unsigned int reg, unsigned int value);
  unsigned int ReadRegister(unsigned int reg);
  u_int32_t ReadMBLT(u_int32_t *&buffer);
  int ConfigureBaselines(vector <unsigned int> &end_values,
			 int nominal_value=16000,
			 int ntries=100);
  int GetClockCounter(u_int32_t timestamp);
  int End();

  int bid(){
    return fBID;
  };

  int LoadDAC(vector<u_int32_t>dac_values);
  
 private:
  int fBoardHandle;
  int fLink, fCrate, fBID;
  unsigned int fBaseAddress;

  // Stuff for clock reset tracking
  u_int32_t clock_counter;
  u_int32_t last_time;
  bool seen_under_5;
  bool seen_over_15;

  MongoLog *fLog;

};


#endif
