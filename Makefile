CPP=g++
LDFLAGS=-lasound
CPPFLAGS=-O3 -g0 -Wall -Wno-format -std=gnu++14 -I./inc
CPP_SOURCES=$(wildcard src/*.cc)
DOCS=index.html

all: $(CPP_SOURCES)
	$(CPP) $(CPP_SOURCES) $(LDFLAGS) $(CPPFLAGS) -o $@
	@doxygen ./mercury.doxyfile
	cp ./docs_FSM/*.png html
	
mercury: $(CPP_SOURCES)
	$(CPP) $(CPP_SOURCES) $(LDFLAGS) $(CPPFLAGS) -o $@


doc: $(CPP_SOURCES)
	@doxygen ./mercury.doxyfile
	cp ./docs_FSM/*.png html

.PHONY: clean install

install:
	install -D mercury /usr/bin/mercury

clean:
	rm -rf mercury
	rm -rf html/*
	rmdir html

