SHELL	= /bin/bash -O extglob -c
CC	= g++
CXX	= g++
CFLAGS	= -Wall -Wextra -pedantic -pedantic-errors -g -DLINUX -std=c++17 -pthread $(shell pkg-config --cflags libmongocxx)
CPPFLAGS := $(CFLAGS)
LDFLAGS = -lCAENVME -lstdc++fs -llz4 -lblosc $(shell pkg-config --libs libmongocxx) $(shell pkg-config --libs libbsoncxx)
LDFLAGS_CC = ${LDFLAGS} -lexpect -ltcl8.6

SOURCES_SLAVE = DAQController.cc main.cc Options.cc MongoLog.cc \
    StraxFormatter.cc V1724.cc V1724_MV.cc V1730.cc Compressor.cc \
    ThreadPool.cc WFSim.cc
OBJECTS_SLAVE = $(SOURCES_SLAVE:%.cc=%.o)
DEPS_SLAVE = $(OBJECTS_SLAVE:%.o=%.d)
EXEC_SLAVE = main

SOURCES_CC = ccontrol.cc Options.cc V2718.cc \
    CControl_Handler.cc DDC10.cc V1495.cc MongoLog.cc
OBJECTS_CC = $(SOURCES_CC:%.cc=%.o)
DEPS_CC = $(OBJECTS_CC:%.o=%.d)
EXEC_CC = ccontrol

all: $(EXEC_SLAVE)

$(EXEC_SLAVE) : $(OBJECTS_SLAVE)
	$(CC) $(OBJECTS_SLAVE) $(CFLAGS) $(LDFLAGS) -o $(EXEC_SLAVE)

ccontrol: $(EXEC_CC)

$(EXEC_CC) : $(OBJECTS_CC)
	$(CC) $(OBJECTS_CC) $(CFLAGS) $(LDFLAGS_CC) -o $(EXEC_CC)

%.d : %.cc
	@set -e; rm -f $@; \
	$(CC) -MM $(CFLAGS) $< > $@.$$$$; \
	sed 's,\($*\)\.o[ :]*,\1.o $@ : ,g' < $@.$$$$ > $@; \
	rm -f $@.$$$$

%.o : %.cc %.d
	$(CC) $(CFLAGS) -o $@ -c $<

include $(DEPS_SLAVE)
include $(DEPS_CC)

.PHONY: clean

clean:
	rm -f *.o *.d
	rm -f $(EXEC_SLAVE)
	rm -f $(EXEC_CC)

