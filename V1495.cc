// ** XENONnT Muon Veto V1495 Board **
// Generic class for writing registers to a CAEN V1495 borad
// Initialisation of the V1724 Muon Veto boards was moved to a dedeicated V1724_MV class

#include <numeric>
#include <iostream>
#include "V1495.hh"
#include "DAXHelpers.hh"
#include "Options.hh"
#include "MongoLog.hh"


V1495::V1495(MongoLog  *log, Options *options, int bid, int handle, unsigned int address){
	fOptions = options;
	fLog = log;
	fBID = bid;
	fBaseAddress = address;
	fBoardHandle = handle;
}

V1495::~V1495(){}

// Kept a separate write registers function for the V1495 here, but in principle can be derived from the V1724 class
int V1495::WriteReg(unsigned int reg, unsigned int value){
	u_int32_t write=0;
	write+=value;
	if(CAENVME_WriteCycle(fBoardHandle, fBaseAddress+reg,
				&write,cvA32_U_DATA,cvD32) != cvSuccess){
		fLog->Entry(MongoLog::Warning, "V1495: %i failed to write register 0x%04x with value %08x (handle %i)", 
				fBID, reg, value, fBoardHandle);
		return -1;
	}
	fLog->Entry(MongoLog::Message, "V1495: %i written register 0x%04x with value %08x (handle %i)",
			fBID, reg, value, fBoardHandle);
	return 0;
}



