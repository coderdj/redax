#include "V1724.hh"

V1724::V1724(){
  fBoardHandle=fLink=fCrate=fBID=-1;
  fBaseAddress=0;
}
V1724::~V1724(){
  End();
}

int V1724::Init(int link, int crate, int bid, unsigned int address=0){  
  if(CAENVME_Init(cvV2718, link, crate, &fBoardHandle)
     != cvSuccess){
    cout<<"Failed to init board"<<endl;
    fBoardHandle = -1;
    return -1;
  }
  fLink = link;
  fCrate = crate;
  fBID = bid;
  fBaseAddress=address;
  cout<<"Successfully initialized board at "<<fBoardHandle<<endl;
  return 0;
}

int V1724::WriteRegister(unsigned int reg, unsigned int value){
  if(CAENVME_WriteCycle(fBoardHandle,fBaseAddress+reg,
			&value,cvA32_U_DATA,cvD32) != cvSuccess){
    cout<<"Failed to write register 0x"<<hex<<reg<<dec<<" to board "<<fBID<<
      " with value "<<hex<<value<<dec<<" board handle "<<fBoardHandle<<endl;
    return -1;
  }
  return 0;
}

unsigned int V1724::ReadRegister(unsigned int reg){
  unsigned int temp;
  if(CAENVME_ReadCycle(fBoardHandle, fBaseAddress+reg, &temp,
		       cvA32_U_DATA, cvD32) != cvSuccess){
    cout<<"Failed to read register 0x"<<hex<<reg<<dec<<" on board "<<fBID<<endl;
    return -1;
  }
  return temp;
}

int V1724::ReadMBLT(unsigned int *buffer){
  // Initialize
  unsigned int blt_bytes=0;
  int nb=0,ret=-5;
  unsigned int BLT_SIZE=8388608; // 8MB buffer size
  buffer = new unsigned int[BLT_SIZE/4];

  int count = 0;
  do{
    ret = CAENVME_FIFOBLTReadCycle(fBoardHandle, fBaseAddress,
				   ((unsigned char*)buffer)+blt_bytes,
				   BLT_SIZE, cvA32_U_BLT, cvD32, &nb);
    if( (ret != cvSuccess) && (ret != cvBusError) ){
      cout<<"Read error in board "<<fBID<<" after "<<count<<" reads."<<endl;
      delete[] buffer;
      return 0;
    }

    count++;
    blt_bytes+=nb;

    if(blt_bytes>BLT_SIZE){
      cout<<"You managed to transfer more data than fits on board."<<endl;
      cout<<"Transferred: "<<blt_bytes<<" bytes, Buffer: "<<BLT_SIZE<<" bytes."<<endl;
      delete[] buffer;
      return 0;
    }
  }while(ret != cvBusError);

  cout<<"Did "<<count<<" BLTs"<<endl;

  return blt_bytes;
  
}

int V1724::ConfigureBaselines(int nominal_value,
			      int ntries,
			      vector <unsigned int> start_values,
			      vector <unsigned int> &end_values){
  cout<<"Not implemented"<<endl;
  return 0;
}
int V1724::End(){
  if(fBoardHandle>=0)
    CAENVME_End(fBoardHandle);
  fBoardHandle=fLink=fCrate=fBID=-1;
  fBaseAddress=0;
  return 0;
}

