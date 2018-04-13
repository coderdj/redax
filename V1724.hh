#ifndef _V1724_HH_
#define _V1724_HH_

#include <unistd.h>
#include <cstring>

#include <stdint.h>
#include <CAENVMElib.h>
#include <vector>

#include <iostream>

using namespace std;

class V1724{

 public:
  V1724();
  ~V1724();

  int Init(int link, int crate, int bid, unsigned int address);
  int WriteRegister(unsigned int register, unsigned int value);
  unsigned int ReadRegister(unsigned int register);
  u_int32_t ReadMBLT(u_int32_t *&buffer);
  int ConfigureBaselines(int nominal_value,
				int ntries,
				vector <unsigned int> start_values,
				vector <unsigned int> &end_values);
  int End();

  int bid(){
    return fBID;
  };

 private:
  int fBoardHandle;
  int fLink, fCrate, fBID;
  unsigned int fBaseAddress;

};


#endif
