#include "f2718.hh"
#include "MongoLog.hh"
#include <sys/socket.h>
#include <arpa/inet.h>

f2718::f2718(MongoLog* log) : V2718(log) {}

f2718::~f2718() {

}

int f2718::SendStartSignal() {
  int sock = 0, valread;
  struct sockaddr_in addr;
  char buffer[1024] = {0};
  if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
    fLog->Error(MongoLog::Error, "Could not create socket, err %i", sock);
    return -1;
  } else {
    fLog->Entry(MongoLog::Local, "Created socket fd %i", sock);
  }
  addr.sin_family = AF_INET;
  addr.sin_port = htons(13178);
  if(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr)<=0){
    fLog->Entry(MongoLog::Error, "Invalid address/ Address not supported");
    return -1;
  }

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0){
    fLog->Entry(MongoLog::Error, "Could not connect");
    return -1;
  }
  send(sock, "start", 6, 0);
  valread = read(sock, buffer, 1024);
  fLog->Entry(MongoLog::Local, "Received %i bytes", valread);
  return 0;
}

int f2718::SendStopSignal(bool) {
  int sock = 0, valread;
  struct sockaddr_in addr;
  char buffer[1024] = {0};
  if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
    fLog->Error(MongoLog::Error, "Could not create socket, err %i", sock);
    return -1;
  } else {
    fLog->Entry(MongoLog::Local, "Created socket fd %i", sock);
  }
  addr.sin_family = AF_INET;
  addr.sin_port = htons(13178);
  if(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr)<=0){
    fLog->Entry(MongoLog::Error, "Invalid address/ Address not supported");
    return -1;
  }

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0){
    fLog->Entry(MongoLog::Error, "Could not connect");
    return -1;
  }
  send(sock, "stop", 5, 0);
  valread = read(sock, buffer, 1024);
  fLog->Entry(MongoLog::Local, "Received %i bytes", valread);
  return 0;
}
