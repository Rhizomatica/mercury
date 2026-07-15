//go:build darwin && mercury_embedded

package main

// macOS link flags for the single-binary Mercury UI, mirroring the daemon's
// Darwin link (CoreAudio/CoreFoundation frameworks, hamlib via pkg-config).
// audioio.a is passed by full path because macOS ld(1) has no GNU "-l:name"
// syntax, and it is not named lib*.a. Linux/Windows use their own _link files.
/*
#cgo pkg-config: hamlib
#cgo LDFLAGS: -L${SRCDIR}/../.. -lmercury_core -L${SRCDIR}/../../modem/freedv -lfreedvdata ${SRCDIR}/../../audioio/audioio.a -framework CoreFoundation -framework CoreAudio -lpthread -lm
*/
import "C"
