# Global definitions for Makefile: OS, compiler, utils

# Undefine built-in rules, suffixes and variables
MAKEFLAGS += -Rr

ifndef OS
	uname := $(shell uname)
	ifeq "$(uname)" "Linux"
		OS := linux
	else ifeq "$(uname)" "FreeBSD"
		OS := freebsd
	else ifeq "$(uname)" "Darwin"
		OS := apple
	endif
else ifeq "$(OS)" "Windows_NT"
	# OS=Windows_NT is default env var on Windows
	OS := windows
endif

DOTEXE :=
SO := so
ifeq "$(OS)" "apple"
	SO := dylib
else ifeq "$(OS)" "windows"
	DOTEXE := .exe
	SO := dll
endif

# Note: Windows: must set CPU=... manually
CPU := amd64
ifneq "$(OS)" "windows"
	CPU := $(shell uname -m)
	ifeq "$(CPU)" "x86_64"
		CPU := amd64
	else ifeq "$(CPU)" "aarch64"
		CPU := arm64
	else ifeq "$(CPU)" "i686"
		CPU := x86
	endif
endif

COMPILER := clang
ifeq "$(OS)" "linux"
	COMPILER := gcc
endif

C := clang -c
CXX := clang++ -c
LINK := clang
LINKXX := clang++
ifeq "$(COMPILER)" "gcc"
	C := $(CROSS_PREFIX)gcc -c -pipe
	CXX := $(CROSS_PREFIX)g++ -c -pipe
	LINK := $(CROSS_PREFIX)gcc -pipe
	LINKXX := $(CROSS_PREFIX)g++ -pipe
endif

CFLAGS :=
CXXFLAGS :=

LINKFLAGS :=
ifeq "$(OS)" "linux"
	LINKFLAGS := -Wl,-no-undefined
else ifeq "$(OS)" "freebsd"
	LINKFLAGS := -Wl,-no-undefined
endif
LINKXXFLAGS := $(LINKFLAGS)

LINK_RPATH_ORIGIN :=
LINK_INSTALLNAME_LOADERPATH :=
ifeq "$(OS)" "linux"
	LINK_RPATH_ORIGIN := '-Wl,-rpath,$$ORIGIN' -Wl,--disable-new-dtags
else ifeq "$(OS)" "freebsd"
	LINK_RPATH_ORIGIN := '-Wl,-rpath,$$ORIGIN'
else ifeq "$(OS)" "apple"
	LINK_INSTALLNAME_LOADERPATH = -Wl,-install_name -Wl,@loader_path/$@
endif

LINK_PTHREAD :=
ifneq "$(OS)" "windows"
	LINK_PTHREAD := -pthread
endif

OBJCOPY := llvm-objcopy
STRIP := llvm-strip
AR := llvm-ar
WINDRES := llvm-windres
ifeq "$(COMPILER)" "gcc"
	OBJCOPY := $(CROSS_PREFIX)objcopy
	STRIP := $(CROSS_PREFIX)strip
	AR := $(CROSS_PREFIX)ar
	WINDRES := $(CROSS_PREFIX)windres
endif

MKDIR := mkdir -p
RM := rm -rf
CP := cp
ifeq "$(OS)" "linux"
	CP := cp -u
endif
SED := sed -i.old
SUBMAKE := +$(MAKE) -f $(firstword $(MAKEFILE_LIST))
