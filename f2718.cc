#include "f2718.hh"
#include "MongoLog.hh"
#include <sys/socket.h>
#include <arpa/inet.h>

f2718::f2718(MongoLog* log) : V2718(log) {}

f2718::~f2718() {

}

int f2718::SendSignal(const std::string& command) {
  int sock = 0, valread;
  struct sockaddr_un addr;
  char buffer[1024] = {0};
  if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) == 0) {
    fLog->Error(MongoLog::Error, "Could not create socket, err %i", sock);
    return -1;
  } else {
    fLog->Entry(MongoLog::Local, "Created socket fd %i", sock);
  }
  addr.sun_family = AF_UNIX;
  addr.sin_path = "./socket";

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0){
    fLog->Entry(MongoLog::Error, "Could not connect");
    return -1;
  }
  send(sock, command.c_str(), command.size(), 0);
  valread = read(sock, buffer, 1024);
  fLog->Entry(MongoLog::Local, "Received %i bytes", valread);
  return 0;
}

int f2718::SendStartSignal() {
  return SendSignal("start");
}

int f2718::SendStopSignal(bool) {
  return SendSignal("stop");
}
