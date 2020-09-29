SHELL	= /bin/bash -O extglob -c
CC	= g++
CXX	= g++
CFLAGS	= -Wall -Wextra -pedantic -pedantic-errors -g -DLINUX -std=c++17 -pthread $(shell pkg-config --cflags libmongocxx)
CPPFLAGS := $(CFLAGS)
LDFLAGS = -lCAENVME -lstdc++fs -llz4 -lblosc $(shell pkg-config --libs libmongocxx) $(shell pkg-config --libs libbsoncxx)
LDFLAGS_CC = ${LDFLAGS} -lexpect -ltcl8.6

SOURCES_SLAVE = $(wildcard *.cc)
OBJECTS_SLAVE = $(SOURCES_SLAVE:%.cc=%.o)
DEPS_SLAVE = $(OBJECTS_SLAVE:%.o=%.d)
EXEC_SLAVE = redax

all: $(EXEC_SLAVE)

$(EXEC_SLAVE) : $(OBJECTS_SLAVE)
	$(CC) $(OBJECTS_SLAVE) $(CFLAGS) $(LDFLAGS) -o $(EXEC_SLAVE)

%.d : %.cc
	@set -e; rm -f $@; \
	$(CC) -MM $(CFLAGS) $< > $@.$$$$; \
	sed 's,\($*\)\.o[ :]*,\1.o $@ : ,g' < $@.$$$$ > $@; \
	rm -f $@.$$$$

%.o : %.cc %.d
	$(CC) $(CFLAGS) -o $@ -c $<

.PHONY: clean

clean:
	rm -f *.o *.d
	rm -f $(EXEC_SLAVE)
	rm -f $(EXEC_CC)

include $(DEPS_SLAVE)

