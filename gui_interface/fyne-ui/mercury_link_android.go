//go:build android && mercury_embedded

package main

// Android link flags for the single-binary Mercury UI.  The core is built
// with the NDK's compiler (make libmercury_core.a CC=aarch64-linux-android26-clang),
// so its audio is AAudio; there is no hamlib, hidapi, pulse or ALSA on
// Android.  GOOS=android also satisfies the linux build constraint, which is
// why the mercury_link_linux*.go files exclude android explicitly.
//
// NOTE: the blank line below is required — without it Go folds this comment
// into the cgo preamble and the C compiler tries to compile these words.

/*
#cgo LDFLAGS: -L${SRCDIR}/../.. -lmercury_core -L${SRCDIR}/../../modem/freedv -lfreedvdata ${SRCDIR}/../../audioio/audioio.a -laaudio -llog -lm
*/
import "C"
