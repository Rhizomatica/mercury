//go:build linux && mercury_embedded

package main

/*
#cgo LDFLAGS: -lhamlib -lpulse -lasound -lpthread -lrt -lm
*/
import "C"
