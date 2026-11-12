//go:build darwin && !ios

package main

// The macOS open panel: see picker_macos.m.

/*
#cgo LDFLAGS: -framework Cocoa
#include <stdint.h>
#include <stdlib.h>
void mercury_pick(int folder, uintptr_t h);
char *mercury_open_bookmark(void *b, int len);
*/
import "C"

import (
	"runtime/cgo"
	"unsafe"
)

// nativePick asks for a file (or a folder) in the system's panel and calls
// done, off the main thread, with the path ("" if cancelled) and, for a
// folder, the bookmark that reopens it after a restart.  False: this system
// has no such panel, and the caller shows its own dialog.
func nativePick(folder bool, done func(path string, bookmark []byte)) bool {
	f := 0
	if folder {
		f = 1
	}
	C.mercury_pick(C.int(f), C.uintptr_t(cgo.NewHandle(done)))
	return true
}

//export mercuryPicked
func mercuryPicked(h C.uintptr_t, path *C.char, bookmark unsafe.Pointer, n C.int) {
	hd := cgo.Handle(h)
	done := hd.Value().(func(string, []byte))
	hd.Delete()
	p := ""
	if path != nil {
		p = C.GoString(path)
	}
	var bm []byte
	if bookmark != nil && n > 0 {
		bm = C.GoBytes(bookmark, n)
	}
	go done(p, bm)
}

// nativeOpenBookmark opens the folder a bookmark from nativePick names and
// returns its path, "" if it no longer resolves.
func nativeOpenBookmark(bookmark []byte) string {
	if len(bookmark) == 0 {
		return ""
	}
	c := C.mercury_open_bookmark(unsafe.Pointer(&bookmark[0]), C.int(len(bookmark)))
	if c == nil {
		return ""
	}
	defer C.free(unsafe.Pointer(c))
	return C.GoString(c)
}
