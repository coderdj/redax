#include "MongoInserter.hh"
#include "DAQController.hh"

MongoInserter::MongoInserter(){
  fOptions = NULL;
  fDataSource = NULL;
  fActive = true;
}

MongoInserter::~MongoInserter(){
}

int MongoInserter::Initialize(Options *options, DAQController *dataSource){
  fOptions = options;
  fDataSource = dataSource;
  return 0;
}

void MongoInserter::Close(){
  fActive = false;
}

int MongoInserter::ReadAndInsertData(){

  std::vector <data_packet> *readVector=NULL;
  int read_length = fDataSource->GetData(readVector);
  while(fActive || read_length>0){

    if(readVector != NULL){
      for(unsigned int i=0; i<readVector->size(); i++){	
	// Just delete
	//std::cout<<(*readVector)[i]->size<<std::endl;
	//for(unsigned int j=0; j<(*readVector)[i]->size/4; j++)
	//std::cout<<hex<<(*readVector)[i]->buff<<std::endl;
	delete[] (*readVector)[i].buff;
      }
      delete readVector;
      readVector=NULL;
    }
    usleep(10000); // 10ms sleep
    read_length = fDataSource->GetData(readVector);
  }
  return 0;  
}
