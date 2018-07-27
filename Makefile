CC	= g++
CFLAGS	= -Wall -g -DLINUX -fPIC -std=c++17 -pthread $(shell pkg-config --cflags libmongocxx)
LDFLAGS = -lCAENVME -lstdc++fs $(shell pkg-config --libs libmongocxx) $(shell pkg-config --libs libbsoncxx)
SOURCES = $(shell echo ./*cc)
OBJECTS = $(SOURCES: .cc=.o)
CPP	= main

all: $(SOURCES) $(CPP)

$(CPP) : $(OBJECTS)
	$(CC) $(OBJECTS) $(CFLAGS) $(LDFLAGS) $(LIBS) -o $(CPP)

clean:
	rm $(CPP)
