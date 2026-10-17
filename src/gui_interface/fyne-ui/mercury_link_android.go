//go:build android && mercury_embedded

package main

// Android link flags for the single-binary Mercury UI.  The core is built
// with the NDK's compiler (make libmercury_core.a CC=aarch64-linux-android26-clang),
// so its audio is AAudio, and its Hamlib is the vendored static one in
// radio_io/hamlib-android (no hidapi, pulse or ALSA on Android).
// GOOS=android also satisfies the linux build constraint, which is why the
// mercury_link_linux*.go files exclude android explicitly.
//
// NOTE: the blank line below is required — without it Go folds this comment
// into the cgo preamble and the C compiler tries to compile these words.

/*
#cgo LDFLAGS: -L${SRCDIR}/../.. -lmercury_core -L${SRCDIR}/../../modem/freedv -lfreedvdata ${SRCDIR}/../../audioio/audioio.a
#cgo android,arm64 LDFLAGS: ${SRCDIR}/../../radio_io/hamlib-android/arm64-v8a/lib/libhamlib.a
#cgo android,amd64 LDFLAGS: ${SRCDIR}/../../radio_io/hamlib-android/x86_64/lib/libhamlib.a
#cgo LDFLAGS: -laaudio -llog -ldl -lm
*/
import "C"
