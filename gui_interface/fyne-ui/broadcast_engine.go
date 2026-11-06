//go:build mercury_embedded

// The broadcast plane through this UI's own engine.
//
// When the chat window is attached to the engine running in this process, its
// broadcast chat and files go through the engine's host calls
// (host/mercury_host.h), as hermes-radio-daemon's do, instead of the broadcast
// TCP port.  That port then stays free for a broadcast client of its own
// (hermes-broadcast, Reticulum).  A remote engine is still reached over TCP.
package main

/*
#include <stdlib.h>
#include <stdint.h>
#include "host/mercury_host.h"

extern void goEngineBroadcastFrame(uint8_t *frame, size_t len);

static void engine_on_frame(const uint8_t *frame, size_t len, int mode, float snr, void *user)
{
    (void)mode; (void)snr; (void)user;
    goEngineBroadcastFrame((uint8_t *)frame, len);
}

static int engine_bcast_rx(int on)
{
    mercury_host_bcast_rx_ops ops = { NULL, engine_on_frame, NULL };
    return mercury_host_set_bcast_rx(on ? &ops : NULL);
}

static const char *engine_bcast_state(mercury_host_bcast_stats_t *st)
{
    mercury_host_bcast_stats(st);
    return st->state;
}
*/
import "C"
import (
	"bytes"
	"encoding/json"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"sync"
	"time"
	"unsafe"
)

// engineIsLocal reports whether host names this machine, where the engine in
// this process listens.
func engineIsLocal(host string) bool {
	if host == "localhost" {
		return true
	}
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}

// ---- chat ------------------------------------------------------------------

// engineSendBroadcastChat sends one chat line at the listen mode.
func engineSendBroadcastChat(line string) error {
	plane := C.CString("bcast")
	defer C.free(unsafe.Pointer(plane))
	text := C.CString(line)
	defer C.free(unsafe.Pointer(text))
	switch C.mercury_host_chat_send(plane, text) {
	case C.MERCURY_HOST_OK:
		return nil
	case C.MERCURY_HOST_TOO_LONG:
		return fmt.Errorf("message too long for one frame at this mode")
	default:
		return fmt.Errorf("the modem refused the message")
	}
}

// engineChatLastSeq is the engine's newest stored chat line.
func engineChatLastSeq() uint64 { return uint64(C.mercury_host_chat_last_seq()) }

// engineChatSince returns the stored chat lines after seq, and the seq of the
// last one returned.
func engineChatSince(seq uint64) ([]HistoryMessage, uint64) {
	buf := make([]byte, 64<<10)
	var msgs []HistoryMessage
	for {
		var last C.uint64_t
		n := int(C.mercury_host_chat_since(C.uint64_t(seq),
			(*C.char)(unsafe.Pointer(&buf[0])), C.size_t(len(buf)), &last))
		if n <= 0 {
			return msgs, seq
		}
		seq = uint64(last)
		for _, line := range bytes.Split(buf[:n], []byte("\n")) {
			var m HistoryMessage
			if len(line) > 0 && json.Unmarshal(line, &m) == nil {
				msgs = append(msgs, m)
			}
		}
	}
}

// ---- received frames ---------------------------------------------------------

// engineRx is what goEngineBroadcastFrame (broadcast_engine_export.go: a file
// that exports may define no C) hands the frames to.
var engineRx struct {
	mu sync.Mutex
	f  func([]byte) bool
}

// engineSetBroadcastFilter hands f every broadcast frame the engine decodes,
// on a thread of the engine's; nil stops it.
func engineSetBroadcastFilter(f func([]byte) bool) {
	engineRx.mu.Lock()
	engineRx.f = f
	engineRx.mu.Unlock()
	on := C.int(0)
	if f != nil {
		on = 1
	}
	C.engine_bcast_rx(on)
}

// ---- sending files -----------------------------------------------------------

// engineBroadcastFile starts the engine's own carousel on the file, at the
// listen mode, and reports its progress until it ends.
func engineBroadcastFile(path string, cycles int, progress func(broadcastFileProgress)) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	if len(data) == 0 {
		return fmt.Errorf("%s is empty", filepath.Base(path))
	}
	name := C.CString(filepath.Base(path))
	defer C.free(unsafe.Pointer(name))
	errBuf := make([]byte, 192)
	rc := C.mercury_host_bcast_send(name, (*C.uint8_t)(unsafe.Pointer(&data[0])), C.size_t(len(data)),
		C.int(cycles), (*C.char)(unsafe.Pointer(&errBuf[0])), C.size_t(len(errBuf)))
	switch rc {
	case C.MERCURY_HOST_OK:
	case C.MERCURY_HOST_BUSY:
		return fmt.Errorf("the modem is busy (an ARQ session, or a broadcast already sending)")
	default:
		return fmt.Errorf("%s", goStringFromC(errBuf))
	}

	go func() {
		tick := time.NewTicker(500 * time.Millisecond)
		defer tick.Stop()
		var reported uint64
		for range tick.C {
			var st C.mercury_host_bcast_stats_t
			state := C.GoString(C.engine_bcast_state(&st))
			pr := broadcastFileProgress{
				CycleNow: int(st.cycle), CyclesTotal: int(st.cycles_total),
				FramesSent: uint64(st.frames_sent),
			}
			switch state {
			case "sending":
				if pr.FramesSent != reported {
					reported = pr.FramesSent
					progress(pr)
				}
				continue
			case "stopped_by_arq":
				pr.Err = fmt.Errorf("an ARQ session started")
			case "error":
				pr.Err = fmt.Errorf("the modem's carousel failed")
			}
			pr.Done = true
			progress(pr)
			return
		}
	}()
	return nil
}

// engineStopBroadcastFile stops the engine's carousel at the next frame.
func engineStopBroadcastFile() { C.mercury_host_bcast_stop() }
