#ifndef _DDC10_HH_
#define _DDC10_HH_

#include "Options.hh"


using namespace std;

class DDC10{

public:
   DDC10();
   ~DDC10();

   int Initialize(HEVOptions d_opts);
   HEVOptions GetHEVOptions(){ return fHopts;};
 
private:
   HEVOptions fHopts;
};
#endif
