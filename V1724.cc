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
  std::cout<<"Writing reg:val: "<<hex<<reg<<":"<<value<<dec<<std::endl;
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

u_int32_t V1724::ReadMBLT(unsigned int *&buffer){
  // Initialize
  unsigned int blt_bytes=0;
  int nb=0,ret=-5;
  unsigned int BLT_SIZE=8*8388608; // 8MB buffer size
  u_int32_t *tempBuffer = new u_int32_t[BLT_SIZE/4];

  int count = 0;
  do{
    ret = CAENVME_FIFOBLTReadCycle(fBoardHandle, fBaseAddress,
				   ((unsigned char*)tempBuffer)+blt_bytes,
				   BLT_SIZE, cvA32_U_BLT, cvD32, &nb);
    if( (ret != cvSuccess) && (ret != cvBusError) ){
      cout<<"Read error in board "<<fBID<<" after "<<count<<" reads."<<endl;
      delete[] tempBuffer;
      return 0;
    }

    count++;
    blt_bytes+=nb;

    if(blt_bytes>BLT_SIZE){
      cout<<"You managed to transfer more data than fits on board."<<endl;
      cout<<"Transferred: "<<blt_bytes<<" bytes, Buffer: "<<BLT_SIZE<<" bytes."<<endl;
      delete[] tempBuffer;
      return 0;
    }
  }while(ret != cvBusError);


  // Now, unfortunately we need to make one copy of the data here or else our memory
  // usage explodes. We declare above a buffer of 8MB, which is the maximum capacity
  // of the board in case every channel is 100% saturated (the practical largest
  // capacity is certainly smaller depending on settings). But if we just keep reserving
  // 8MB blocks and filling 500kB with actual data, we're gonna run out of memory.
  // So here we declare the return buffer as *just* large enough to hold the actual
  // data and free up the rest of the memory reserved as buffer.
  // In tests this does not seem to impact our ability to read out the V1724 at the
  // maximum bandwidth of the link.
  if(blt_bytes>0){
    buffer = new u_int32_t[blt_bytes/(sizeof(u_int32_t))];
    std::memcpy(buffer, tempBuffer, blt_bytes);
  }
  delete[] tempBuffer;
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

