//go:build mercury_embedded

// CGo bridge to the Mercury C engine.
// mercury_bridge.c is compiled alongside this file by CGo.
// Pre-built Mercury objects live in libmercury_core.a.

package main

/*
#cgo CFLAGS: -I${SRCDIR}/../.. -I${SRCDIR}/../../modem/freedv -I${SRCDIR}/../../modem -I${SRCDIR}/../../datalink_broadcast -I${SRCDIR}/../../data_interfaces -I${SRCDIR}/../../datalink_arq -I${SRCDIR}/../../audioio/ffaudio -I${SRCDIR}/../../common -I${SRCDIR}/../../gui_interface -I${SRCDIR}/../../radio_io -I${SRCDIR}/../../common/iniparser -I${SRCDIR}/engine -pthread -D_GNU_SOURCE

#include <stdlib.h>
#include "mercury_bridge.h"
*/
import "C"
import (
	"fmt"
	"unsafe"
)

// mercuryInfoCheck forwards the args to the engine's CLI parser and handles the
// informational actions (-h/-l/-z/-K) by printing to the terminal. Returns true
// if such an action was handled and the process should exit before the GUI.
func mercuryInfoCheck(args []string) bool {
	cArgs := make([]*C.char, len(args))
	for i, a := range args {
		cArgs[i] = C.CString(a)
	}
	defer func() {
		for _, p := range cArgs {
			C.free(unsafe.Pointer(p))
		}
	}()
	var argv **C.char
	if len(cArgs) > 0 {
		argv = &cArgs[0]
	}
	cDefault := C.CString("")
	defer C.free(unsafe.Pointer(cDefault))
	return C.mercury_precheck(C.int(len(args)), argv, cDefault) != 0
}

func mercuryStart(defaultConfig, logPath string, args []string) error {
	// Marshal the Go args into a C argv (char**) so the engine's own CLI
	// parser handles them — one source of truth with the standalone daemon.
	cArgs := make([]*C.char, len(args))
	for i, a := range args {
		cArgs[i] = C.CString(a)
	}
	defer func() {
		for _, p := range cArgs {
			C.free(unsafe.Pointer(p))
		}
	}()

	cDefault := C.CString(defaultConfig)
	cLog := C.CString(logPath)
	defer C.free(unsafe.Pointer(cDefault))
	defer C.free(unsafe.Pointer(cLog))

	var argv **C.char
	if len(cArgs) > 0 {
		argv = &cArgs[0]
	}

	if C.mercury_init(C.int(len(args)), argv, cDefault, cLog) != 0 {
		return fmt.Errorf("mercury engine init failed")
	}
	return nil
}

func mercuryStop() {
	C.mercury_request_shutdown()
	C.mercury_shutdown()
}
