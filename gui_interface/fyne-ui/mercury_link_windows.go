//go:build windows && mercury_embedded

package main

/*
#cgo LDFLAGS: -L${SRCDIR}/../../radio_io/hamlib-w64/lib -lhamlib -lole32 -ldsound -ldxguid -lws2_32 -static-libgcc -static-libstdc++ -lwinpthread -lm
*/
import "C"
