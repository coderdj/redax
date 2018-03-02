#ifndef _V1724_HH_
#define _V1724_HH_

//#include <stdlib.h>
//#include <cstdio>
#include <unistd.h>


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
  int ReadMBLT(u_int32_t *buffer);
  int ConfigureBaselines(int nominal_value,
				int ntries,
				vector <unsigned int> start_values,
				vector <unsigned int> &end_values);
  int End();

 private:
  int fBoardHandle;
  int fLink, fCrate, fBID;
  unsigned int fBaseAddress;
};


#endif
