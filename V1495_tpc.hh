#ifndef _V1495_TPC_HH_
#define _V1495_TPC_HH_

#include "V1495.hh"

class V1495_TPC : public V1495 {
  public:
    V1495_TPC(std::shared_ptr<MongoLog>&, std::shared_ptr<Options>&, int, int, unsigned);
    virtual ~V1495_TPC();
    virtual int Arm(std::map<std::string, int>&);
    virtual int BeforeSINStart();
    virtual int AfterSINStop();

  private:
    const uint32_t fControlReg
    const uint32_t fVetoOffMSBReg;
    const uint32_t fVetoOffLSBReg;
    const uint32_t fVetoOnMSBReg;
    const uint32_t fVetoOnLSBReg;

    int fFractionalModeActive;
    long fVetoOn_clk, fVetoOff_clk;
};

#endif // _V1495_TPC_HH_ defined
