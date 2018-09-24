SHELL   = /bin/bash -O extglob -c
CC	= g++
CFLAGS	= -Wall -g -DLINUX -fPIC -std=c++17 -pthread $(shell pkg-config --cflags libmongocxx)
LDFLAGS = -lCAENVME -lstdc++fs -lblosc $(shell pkg-config --libs libmongocxx) $(shell pkg-config --libs libbsoncxx)
SOURCES_SLAVE = $(shell echo !(ccontrol|CControl*)+(.cc))
OBJECTS_SLAVE = $(SOURCES_SLAVE: .cc=.o)
CPP_SLAVE = main
EXEC_SLAVE = main

SOURCES_CC = $(shell echo !(main|Strax*|DAQController|*Inserter|V1724)+(.cc))
OBJECTS_CC = $(SOURCES_CC: .cc=.o)
CPP_CC = ccontrol

all: $(SOURCES_SLAVE) $(CPP_SLAVE)

$(CPP_SLAVE) : $(OBJECTS_SLAVE)
	$(CC) $(OBJECTS_SLAVE) $(CFLAGS) $(LDFLAGS) -o $(EXEC_SLAVE)


ccontrol: $(SOURCES_CC) $(CPP_CC)

$(CPP_CC) : $(OBJECTS_CC)
	$(CC) $(OBJECTS_CC) $(CFLAGS) $(LDFLAGS) $(LIBS) -o $(CPP_CC)

clean:
	rm $(CPP_SLAVE)
	rm $(CPP_CC)
