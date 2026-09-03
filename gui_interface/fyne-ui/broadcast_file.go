//go:build mercury_embedded

// Broadcast file transmission.
//
// RaptorQ framing is done in C (datalink_broadcast/bcast_file.c, shared with
// hermes-broadcast); this file is only the pump. It pulls one frame at a time
// and writes it to the SAME broadcast socket the chat client already holds, so
// chat and file transfer share one transport and one set of framing rules.
//
// There is no return path on a broadcast, so nothing here waits for anything:
// the file is repeated as a carousel and a receiver decodes as soon as it has
// collected enough distinct symbols, whenever it happened to start listening.
package main

/*
#include <stdlib.h>
#include "mercury_bridge.h"
*/
import "C"
import (
	"fmt"
	"sync"
	"unsafe"
)

// broadcastFileMaxBytes is the largest file we will transmit: a floppy disk.
// The file is held in memory, and a carousel over HF is slow enough that
// anything larger is a mistake to refuse up front rather than discover hours in.
func broadcastFileMaxBytes() int64 { return int64(C.mercury_bcast_max_file_bytes()) }

// broadcastFileProgress is what the UI renders while a transfer runs.
type broadcastFileProgress struct {
	CycleNow    int
	CyclesTotal int // 0 = until stopped
	FramesSent  uint64
	FileBytes   int64
	Blocks      int
	FrameSize   int
	Done        bool
	Err         error
}

// broadcastSender is the transport this needs: satisfied by the chat window's
// mercury client, so the file rides the connection chat already opened.
type broadcastSender interface {
	SendBroadcastFrame(frame []byte) error
}

// broadcastFileTx is one running transmission. Safe to Stop from the UI thread
// while the pump goroutine is running.
type broadcastFileTx struct {
	mu      sync.Mutex
	handle  unsafe.Pointer
	stopped bool
}

// newBroadcastFileTx opens a file for transmission. cycles == 0 repeats until
// stopped. It does not send anything; call Run.
func newBroadcastFileTx(path string, mode, cycles int) (*broadcastFileTx, error) {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))

	errBuf := make([]byte, 192)
	// session_id 0 asks the encoder to pick one; it is carried in every frame
	// so a receiver can tell this file from the last one it finished.
	h := C.mercury_bcast_tx_open(cPath, C.int(mode), C.int(cycles), 0,
		(*C.char)(unsafe.Pointer(&errBuf[0])), C.int(len(errBuf)))
	if h == nil {
		return nil, fmt.Errorf("%s", goStringFromC(errBuf))
	}
	return &broadcastFileTx{handle: h}, nil
}

func (t *broadcastFileTx) info() (frameSize int, fileBytes int64, blocks int) {
	t.mu.Lock()
	defer t.mu.Unlock()
	if t.handle == nil {
		return 0, 0, 0
	}
	var fb C.long
	var bl C.int
	C.mercury_bcast_tx_source(t.handle, &fb, &bl)
	return int(C.mercury_bcast_tx_frame_size(t.handle)), int64(fb), int(bl)
}

// Stop ends the transmission at the next frame boundary. Idempotent.
func (t *broadcastFileTx) Stop() {
	t.mu.Lock()
	t.stopped = true
	t.mu.Unlock()
}

// close releases the C handle. Must not race with next().
func (t *broadcastFileTx) close() {
	t.mu.Lock()
	defer t.mu.Unlock()
	if t.handle != nil {
		C.mercury_bcast_tx_close(t.handle)
		t.handle = nil
	}
}

// next fills buf with the next frame. Returns 0 when the run is complete.
func (t *broadcastFileTx) next(buf []byte) (int, bool, error) {
	t.mu.Lock()
	defer t.mu.Unlock()
	if t.stopped || t.handle == nil {
		return 0, true, nil
	}
	n := int(C.mercury_bcast_tx_next(t.handle,
		(*C.uchar)(unsafe.Pointer(&buf[0])), C.int(len(buf))))
	switch {
	case n < 0:
		return 0, true, fmt.Errorf("RaptorQ encoder failed")
	case n == 0:
		return 0, true, nil // cycles complete
	}
	return n, false, nil
}

func (t *broadcastFileTx) stats() (cycleNow, cyclesTotal int, frames uint64) {
	t.mu.Lock()
	defer t.mu.Unlock()
	if t.handle == nil {
		return 0, 0, 0
	}
	var cn, ct C.int
	var fs C.ulonglong
	C.mercury_bcast_tx_stats(t.handle, &cn, &ct, &fs)
	return int(cn), int(ct), uint64(fs)
}

// Run pumps frames to the sender until the cycle budget is spent, Stop is
// called, or the transport fails. It reports progress after every frame and
// always reports a final Done. Call it in a goroutine; it closes the handle.
func (t *broadcastFileTx) Run(s broadcastSender, progress func(broadcastFileProgress)) {
	defer t.close()

	frameSize, fileBytes, blocks := t.info()
	report := func(err error, done bool) {
		if progress == nil {
			return
		}
		cn, ct, fr := t.stats()
		progress(broadcastFileProgress{
			CycleNow: cn, CyclesTotal: ct, FramesSent: fr,
			FileBytes: fileBytes, Blocks: blocks, FrameSize: frameSize,
			Done: done, Err: err,
		})
	}

	if frameSize <= 0 {
		report(fmt.Errorf("transmission was not opened"), true)
		return
	}

	buf := make([]byte, frameSize)
	for {
		n, done, err := t.next(buf)
		if err != nil {
			report(err, true)
			return
		}
		if done {
			report(nil, true)
			return
		}
		if err := s.SendBroadcastFrame(buf[:n]); err != nil {
			// The socket went away -- most likely another client took the
			// broadcast port, which the TNC hands over without warning.
			report(fmt.Errorf("broadcast send failed: %w", err), true)
			return
		}
		report(nil, false)
	}
}

// broadcastModeUsable reports whether a Mercury mode can carry broadcast at
// all. DATAC14's 3-byte frame cannot hold the 9-byte configuration packet.
func broadcastModeUsable(mode int) bool {
	return C.mercury_bcast_mode_usable(C.int(mode)) != 0
}

// broadcastEngineMode is the hermes mode index the engine is running, or -1 if
// it cannot carry broadcast.  Fixed at startup by -m; there is no runtime
// switch, and the far station must be set to the same one.
func broadcastEngineMode() int {
	return int(C.mercury_bcast_engine_mode())
}

// broadcastEngineBitrate and broadcastEngineBandwidth describe the running
// modem, computed from the modem itself rather than a table, so they cannot
// disagree with what is actually on the air.
func broadcastEngineBitrate() int     { return int(C.mercury_bcast_engine_bitrate()) }
func broadcastEngineBandwidthHz() int { return int(C.mercury_bcast_engine_bandwidth_hz()) }

// broadcastModeName is the mode's name as `mercury -l` reports it (DATAC3,
// QAM16C2 ...).  An operator matching two stations reads names, not indices.
func broadcastModeName(mode int) string {
	return C.GoString(C.mercury_bcast_mode_name(C.int(mode)))
}

// broadcastModeFrameSize is the payload bytes per modem frame for a mode.
func broadcastModeFrameSize(mode int) int {
	return int(C.mercury_bcast_mode_frame_size(C.int(mode)))
}

// ---- Receiving ------------------------------------------------------------

// broadcastRxResult mirrors bcast_rx_result_t.
type broadcastRxResult int

const (
	broadcastRxIgnored  broadcastRxResult = 0
	broadcastRxProgress broadcastRxResult = 1
	broadcastRxComplete broadcastRxResult = 2
	broadcastRxError    broadcastRxResult = 3
)

// broadcastFileRxProgress is what the UI renders while a file is arriving.
type broadcastFileRxProgress struct {
	Symbols     uint64
	ExpectBytes int64
	Name        string // set on completion
	Path        string // set on completion
	Err         error
}

// broadcastFileRx decodes incoming broadcast file frames.
//
// Frames arrive on the chat client's broadcast socket, so this is fed from that
// reader rather than opening a second connection: Mercury's broadcast port
// accepts exactly one client.
type broadcastFileRx struct {
	mu     sync.Mutex
	handle unsafe.Pointer
}

func newBroadcastFileRx(mode int, dir string) (*broadcastFileRx, error) {
	cDir := C.CString(dir)
	defer C.free(unsafe.Pointer(cDir))
	errBuf := make([]byte, 192)
	h := C.mercury_bcast_rx_open(C.int(mode), cDir,
		(*C.char)(unsafe.Pointer(&errBuf[0])), C.int(len(errBuf)))
	if h == nil {
		return nil, fmt.Errorf("%s", goStringFromC(errBuf))
	}
	return &broadcastFileRx{handle: h}, nil
}

// Frame feeds one raw broadcast frame in. Returns whether the frame was ours
// (so the caller can keep it out of chat) and, when a file completes or fails,
// what happened.
func (r *broadcastFileRx) Frame(frame []byte) (claimed bool, pr broadcastFileRxProgress) {
	if len(frame) == 0 {
		return false, pr
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.handle == nil {
		return false, pr
	}

	res := broadcastRxResult(C.mercury_bcast_rx_frame(r.handle,
		(*C.uchar)(unsafe.Pointer(&frame[0])), C.int(len(frame))))
	if res == broadcastRxIgnored {
		return false, pr
	}

	var sym C.ulonglong
	var want C.long
	C.mercury_bcast_rx_stats(r.handle, &sym, &want)
	pr.Symbols = uint64(sym)
	pr.ExpectBytes = int64(want)

	switch res {
	case broadcastRxComplete:
		pr.Name = C.GoString(C.mercury_bcast_rx_last_name(r.handle))
		pr.Path = C.GoString(C.mercury_bcast_rx_last_path(r.handle))
	case broadcastRxError:
		pr.Err = fmt.Errorf("%s", C.GoString(C.mercury_bcast_rx_error(r.handle)))
	}
	return true, pr
}

func (r *broadcastFileRx) Close() {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.handle != nil {
		C.mercury_bcast_rx_close(r.handle)
		r.handle = nil
	}
}
