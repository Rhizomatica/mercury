//go:build mercury_embedded

// CGo bridge to the Mercury C engine.
// mercury_bridge.c is compiled alongside this file by CGo.
// Pre-built Mercury objects live in libmercury_core.a.

package main

/*
#cgo CFLAGS: -I${SRCDIR}/../.. -I${SRCDIR}/../../modem/freedv -I${SRCDIR}/../../modem -I${SRCDIR}/../../datalink_broadcast -I${SRCDIR}/../../data_interfaces -I${SRCDIR}/../../datalink_arq -I${SRCDIR}/../../audioio/ffaudio -I${SRCDIR}/../../common -I${SRCDIR}/../../gui_interface -I${SRCDIR}/../../radio_io -I${SRCDIR}/../../common/iniparser -I${SRCDIR}/engine -pthread -D_GNU_SOURCE
#cgo LDFLAGS: -L${SRCDIR}/../.. -lmercury_core

#include <stdlib.h>
#include "mercury_bridge.h"
*/
import "C"
import (
	"fmt"
	"unsafe"
)

func mercuryStart(configPath, logPath string, verbose bool) error {
	v := 0
	if verbose {
		v = 1
	}
	cConfig := C.CString(configPath)
	cLog := C.CString(logPath)
	defer C.free(unsafe.Pointer(cConfig))
	defer C.free(unsafe.Pointer(cLog))

	if C.mercury_init(cConfig, cLog, C.int(v)) != 0 {
		return fmt.Errorf("mercury engine init failed")
	}
	return nil
}

func mercuryStop() {
	C.mercury_request_shutdown()
	C.mercury_shutdown()
}
