//go:build ios && mercury_embedded

package main

// iOS link flags for the single-binary Mercury UI.  The core is built with an
// iOS compiler (make libmercury_core.a CC="xcrun -sdk iphoneos clang ..."), so
// its audio is the RemoteIO backend (AudioToolbox) with the AVFoundation
// session; there is no hamlib, no IOKit HID and no serial port on iOS.
// GOOS=ios also satisfies the darwin build constraint, which is why
// mercury_link_darwin.go excludes ios explicitly.
//
// NOTE: the blank line below is required — without it Go folds this comment
// into the cgo preamble and the C compiler tries to compile these words.

/*
#cgo LDFLAGS: -L${SRCDIR}/../.. -lmercury_core -L${SRCDIR}/../../modem/freedv -lfreedvdata ${SRCDIR}/../../audioio/audioio.a -framework AudioToolbox -framework AVFoundation -framework Foundation -framework CoreFoundation -lpthread -lm
*/
import "C"
