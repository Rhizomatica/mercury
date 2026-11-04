//go:build hybris && linux

package main

/*
// libhybris runs the Android GPU driver with bionic, which keeps its TLS
// slots right after the thread pointer (tp[2..7] on arm64) -- where glibc
// puts the executable's first static TLS block, and so Go's goroutine
// pointer (runtime.tls_g).  The driver overwrites it during
// glfwCreateWindow and the next cgo call faults.  Initialised TLS
// (.tdata) is laid out before .tbss, so this moves tls_g past the slots.
// Only its last byte is set: the slots themselves must read as zero, bionic
// follows a non-zero slot as a pointer.
__thread char mercury_hybris_tls_pad[128] __attribute__((used, aligned(16))) = { [127] = 1 };
*/
import "C"
