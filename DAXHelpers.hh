#ifndef _DAXHELPERS_HH_
#define _DAXHELPERS_HH_

#include <string>
#include <sstream>
#include <iostream>

class DAXHelpers{
  /* 
     Class to include simple helper functions such as
     unit conversions and the like.
  */
  
public:
  DAXHelpers(){};
  ~DAXHelpers(){};

static unsigned int StringToHex(std::string str){
    std::stringstream ss(str);
    u_int32_t result;
    return ss >> std::hex >> result ? result : 0;
};

  const static int Idle    = 0;
  const static int Arming  = 1;
  const static int Armed   = 2;
  const static int Running = 3;
  const static int Error   = 4;
  const static int Unknown = 5;
  
private:

};

#endif
