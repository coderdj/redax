#ifndef _DAXHELPERS_HH_
#define _DAXHELPERS_HH_

#include <string>
#include <sstream>
#include <chrono>
#include <iomanip> //put_time
#include <ctime>   //local time
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

static std::string IntToString(const int num)
{
   std::ostringstream convert;
   convert<<num;
   return convert.str();
}

static std::string GetChronoTimeString()
{
  auto now = std::chrono::system_clock::now();
  auto in_time_t = std::chrono::system_clock::to_time_t(now);
  
  std::stringstream ss;
  ss << std::put_time(std::localtime(&in_time_t),"%Y-%m-%d %X");
  //ss << in_time_t;
  return ss.str();
}

private:

};

#endif
