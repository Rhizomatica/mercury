//go:build mercury_embedded

package main

/*
#include <stddef.h>
#include <stdint.h>
*/
import "C"
import "unsafe"

// goEngineBroadcastFrame is the engine's on_frame (broadcast_engine.go).
//
//export goEngineBroadcastFrame
func goEngineBroadcastFrame(frame *C.uint8_t, n C.size_t) {
	engineRx.mu.Lock()
	f := engineRx.f
	engineRx.mu.Unlock()
	if f != nil && n > 0 {
		f(C.GoBytes(unsafe.Pointer(frame), C.int(n)))
	}
}
