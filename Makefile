# Mercury Modem
#
#    This program is free software: you can redistribute it and/or modify
#    it under the terms of the GNU Affero General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU Affero General Public License for more details.
#
#    You should have received a copy of the GNU Affero General Public License
#    along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
#

# GUI build option (1=enabled by default, 0=headless only)
GUI_ENABLED ?= 1

ifeq ($(OS),Windows_NT)
	FFAUDIO_LINKFLAGS += -lole32
	FFAUDIO_LINKFLAGS += -ldsound -ldxguid
	FFAUDIO_LINKFLAGS += -lws2_32
	FFAUDIO_LINKFLAGS += -static-libgcc -static-libstdc++ -static -l:libwinpthread.a
else
	UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Linux)
		FFAUDIO_LINKFLAGS += -lpulse
		FFAUDIO_LINKFLAGS += -lasound -lpthread -lrt
    endif
    ifeq ($(UNAME_S),Darwin)
		FFAUDIO_LINKFLAGS := -framework CoreFoundation -framework CoreAudio
    endif
    ifeq ($(UNAME_S),FreeBSD)
		FFAUDIO_LINKFLAGS := -lm
    endif
endif

#WATCOM_CFLAGS="-bm"
#GCC_CFLAGS="-lpthread -lrt"

CPP=g++
LDFLAGS=$(FFAUDIO_LINKFLAGS)
CPPFLAGS=-O3 -g0 -Wall -Wextra -Wno-format -Wno-unused -std=c++14 -I./include -I./source/audioio/ffaudio -pthread
#CPPFLAGS=-Ofast -g0 -Wall -fstack-protector -D_FORTIFY_SOURCE=2 -Wno-format -std=gnu++14 -I./include
CPP_SOURCES=$(wildcard source/*.cc source/datalink_layer/*.cc source/physical_layer/*.cc source/common/*.cc)
OBJECT_FILES=$(patsubst %.cc,%.o,$(CPP_SOURCES))

# ========== GUI Build Configuration ==========
ifeq ($(GUI_ENABLED),1)
    CPPFLAGS += -DMERCURY_GUI_ENABLED
    CPPFLAGS += -I./third_party/imgui -I./third_party/imgui/backends

    # ImGui core sources (GLFW + OpenGL3 backend)
    IMGUI_DIR = third_party/imgui
    IMGUI_SOURCES = $(IMGUI_DIR)/imgui.cpp \
                    $(IMGUI_DIR)/imgui_draw.cpp \
                    $(IMGUI_DIR)/imgui_tables.cpp \
                    $(IMGUI_DIR)/imgui_widgets.cpp \
                    $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp \
                    $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp
    IMGUI_OBJECTS = $(patsubst %.cpp,%.o,$(IMGUI_SOURCES))

    # Mercury GUI sources
    GUI_SOURCES = $(wildcard source/gui/*.cc source/gui/widgets/*.cc source/gui/dialogs/*.cc)
    GUI_OBJECTS = $(patsubst %.cc,%.o,$(GUI_SOURCES))

    # Add to object files
    OBJECT_FILES += $(GUI_OBJECTS)

    ifeq ($(OS),Windows_NT)
        # Windows: GLFW (bundled static lib) + OpenGL32 + GDI32
        CPPFLAGS += -I./third_party/glfw/include
        LDFLAGS += -L./third_party/glfw/lib -lglfw3 -lopengl32 -lgdi32
    else ifeq ($(UNAME_S),Darwin)
        # macOS: GLFW via pkg-config + OpenGL framework
        CPPFLAGS += $(shell pkg-config --cflags glfw3)
        LDFLAGS += $(shell pkg-config --libs glfw3) -framework OpenGL
    else
        # Linux: GLFW via pkg-config + GL
        CPPFLAGS += $(shell pkg-config --cflags glfw3)
        LDFLAGS += $(shell pkg-config --libs glfw3) -lGL
    endif
endif

DOCS=index.html

uname_m := $(shell uname -m)
ifeq (${uname_m},aarch64)
# Raspberry Pi 4 compiler flags:
	CPPFLAGS+=-march=armv8-a+crc
# Raspberry Pi 5 compiler flags (comment above and uncomment below):
#	CPPFLAGS+=-march=armv8.2-a+crypto+fp16+rcpc+dotprod
endif

.PHONY: clean install examples audioio

all: mercury examples

examples:
	$(MAKE) -C examples

source/audioio/audioio.a: source/audioio/audioio.c
	$(MAKE) -C source/audioio GUI_ENABLED=$(GUI_ENABLED)

# Main executable - conditionally include ImGui objects
ifeq ($(GUI_ENABLED),1)
mercury: $(OBJECT_FILES) $(IMGUI_OBJECTS) source/audioio/audioio.a
	$(CPP) -o $@ $(OBJECT_FILES) $(IMGUI_OBJECTS) source/audioio/audioio.a $(LDFLAGS)
else
mercury: $(OBJECT_FILES) source/audioio/audioio.a
	$(CPP) -o $@ $(OBJECT_FILES) source/audioio/audioio.a $(LDFLAGS)
endif

# Mercury source files
%.o : %.cc %.h
	$(CPP) -c $(CPPFLAGS) $< -o $@

# ImGui source files (C++)
ifeq ($(GUI_ENABLED),1)
$(IMGUI_DIR)/%.o: $(IMGUI_DIR)/%.cpp
	$(CPP) -c $(CPPFLAGS) -Wno-cast-function-type $< -o $@

$(IMGUI_DIR)/backends/%.o: $(IMGUI_DIR)/backends/%.cpp
	$(CPP) -c $(CPPFLAGS) -Wno-cast-function-type $< -o $@
endif

doc: $(CPP_SOURCES)
	@doxygen ./mercury.doxyfile
	cp ./docs_FSM/*.png html


install: mercury
	install -D mercury $(DESTDIR)/usr/bin/mercury
	install -m 644 -D systemd/modem.service $(DESTDIR)/etc/systemd/system/modem.service

clean:
	rm -rf mercury mercury.exe $(OBJECT_FILES)
	rm -rf html/
ifeq ($(GUI_ENABLED),1)
	rm -rf $(IMGUI_OBJECTS) $(GUI_OBJECTS)
endif
	$(MAKE) -C examples clean
	$(MAKE) -C source/audioio clean
