//go:build linux && mercury_embedded

package main

/*
#cgo LDFLAGS: -L${SRCDIR}/../.. -lmercury_core -L${SRCDIR}/../../modem/freedv -lfreedvdata -L${SRCDIR}/../../audioio -l:audioio.a -lhamlib -lpulse -lasound -lpthread -lrt -lm
*/
import "C"
