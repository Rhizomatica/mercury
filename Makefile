CPP=g++
LDFLAGS=-lasound
CPPFLAGS=-O3 -g0 -Wall -Wno-format -std=gnu++14 -I./include
CPP_SOURCES=$(wildcard source/*.cc)

mercury: $(CPP_SOURCES)
	$(CPP) $(CPP_SOURCES) $(LDFLAGS) $(CPPFLAGS) -o $@


.PHONY: clean install

install:
	install -D mercury /usr/bin/mercury

clean:
	rm -rf mercury
