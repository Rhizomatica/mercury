CPP=g++
LDFLAGS=-lasound
CPPFLAGS=-g -Wall -std=gnu++14 -I./include
CPP_SOURCES=$(wildcard source/*.cc)


all: mercury

mercury: $(CPP_SOURCES)
		$(CPP) $(CPP_SOURCES) $(LDFLAGS) $(CPPFLAGS) -o $@

clean:
		rm -rf mercury
