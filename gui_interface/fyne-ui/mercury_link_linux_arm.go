//go:build linux && !android && mercury_embedded && arm

package main

// 32-bit ARM (GOARCH=arm, e.g. Raspberry Pi OS 32-bit) has no native 64-bit
// atomic instructions, so the C engine's 64-bit _Atomic operations lower to
// libatomic calls (__atomic_load_8, __atomic_store_8, ...).  Link libatomic
// there; 64-bit targets inline these and do not need it.
//
// NOTE: the blank line below is required — without it Go folds this comment
// into the cgo preamble and the C compiler tries to compile these words.

/*
#cgo LDFLAGS: -latomic
*/
import "C"
