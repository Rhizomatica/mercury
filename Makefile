CPP=g++
LDFLAGS=-lasound
CPPFLAGS=-O3 -g0 -Wall -Wno-format -std=gnu++14 -I./include
CPP_SOURCES=$(wildcard source/*.cc)
DOCS=index.html

all: mercury doc
	$(CPP) $(CPP_SOURCES) $(LDFLAGS) $(CPPFLAGS) -o mercury
	@doxygen ./mercury.doxyfile
	cp ./docs_FSM/*.png html

mercury: $(CPP_SOURCES)
	$(CPP) $(CPP_SOURCES) $(LDFLAGS) $(CPPFLAGS) -o $@


doc: $(CPP_SOURCES)
	@doxygen ./mercury.doxyfile
	cp ./docs_FSM/*.png html

.PHONY: clean install

install: mercury
	install -D mercury /usr/bin/mercury

clean:
	rm -rf mercury
	rm -rf html/*
	rmdir html

