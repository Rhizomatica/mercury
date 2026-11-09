//go:build mercury_embedded

// The chat window's TNC client for this UI's own engine.
//
// engineTNC does what client.Client does over TCP, in-process: the control
// port's own commands through mercury_host_tnc_command, ARQ chat as the
// session's bytes (mercury_host_arq_read, cut into lines as client.Client
// cuts the data port's), broadcast chat from the engine's chat store, and the
// session followed from the window's telemetry.  It
// holds the TNC's one client slot as a TCP client would, and a TCP client
// that connects (uucp, VarAC) takes it, ending the session, as it takes it
// from another TCP client.  A remote engine is still reached over TCP.
package main

/*
#include <stdlib.h>
#include <stdint.h>
#include "host/mercury_host.h"
#include "mercury_bridge.h"
*/
import "C"
import (
	"fmt"
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"

	"mercury-client/client"
)

// engineTNCPoll is how often the session's bytes, its state and the chat store
// are read.
const engineTNCPoll = 250 * time.Millisecond

type engineTNC struct {
	cfg client.Config

	log       chan string
	status    chan string
	arqChat   chan client.ChatMessage
	bcastChat chan client.ChatMessage

	mu      sync.Mutex
	done    chan struct{}
	watched chan struct{} // closed when watch has returned
	leaving sync.Mutex    // one Disconnect at a time, to its end

	evicted atomic.Bool // a TCP client took the slot
	sync    atomic.Bool // an ARQ session is up
}

func newEngineTNC(cfg client.Config) *engineTNC {
	if cfg.BandwidthHz == 0 {
		cfg.BandwidthHz = 2300
	}
	return &engineTNC{
		cfg:       cfg,
		log:       make(chan string, 256),
		status:    make(chan string, 64),
		arqChat:   make(chan client.ChatMessage, 256),
		bcastChat: make(chan client.ChatMessage, 256),
	}
}

func (e *engineTNC) Events() client.Events {
	return client.Events{Log: e.log, ARQChat: e.arqChat, BroadcastChat: e.bcastChat, Status: e.status}
}

// command runs one control-port line in the engine.
func (e *engineTNC) command(line string) error {
	cl := C.CString(line)
	defer C.free(unsafe.Pointer(cl))
	switch C.mercury_host_tnc_command(cl) {
	case C.MERCURY_HOST_OK:
		return nil
	case C.MERCURY_HOST_BUSY:
		return fmt.Errorf("another client (uucp, VarAC, RNS...) is attached to the TNC ports")
	default:
		return fmt.Errorf("the modem refused %q", strings.TrimSpace(line))
	}
}

// Connect takes the TNC with what client.Client sends when it connects.
func (e *engineTNC) Connect() error {
	for _, line := range []string{
		"MYCALL " + e.cfg.MyCallsign, "LISTEN ON", "PUBLIC OFF", "COMPRESSION OFF",
		fmt.Sprintf("BW%d", e.cfg.BandwidthHz),
	} {
		if err := e.command(line); err != nil {
			C.mercury_host_tnc_release()
			return err
		}
	}
	e.mu.Lock()
	e.done = make(chan struct{})
	e.watched = make(chan struct{})
	done, watched := e.done, e.watched
	e.mu.Unlock()
	e.evicted.Store(false)
	e.sync.Store(false)
	e.say("Connected to the modem in this program.")
	liveEngineTNC.Store(e)
	go func() {
		defer close(watched)
		e.watch(done, engineChatLastSeq())
	}()
	return nil
}

// Disconnect gives the TNC back, as a TCP client's leaving does.  It returns
// once watch has stopped, so nothing reads the engine's ring after it: the
// app's shutdown disconnects through it before the engine frees the ring.
func (e *engineTNC) Disconnect() {
	// The window's and the app's shutdown may both disconnect: the second
	// returns only once the first has given the TNC back.
	e.leaving.Lock()
	defer e.leaving.Unlock()
	e.mu.Lock()
	done, watched := e.done, e.watched
	e.done = nil
	e.mu.Unlock()
	if done == nil {
		return
	}
	close(done)
	<-watched
	liveEngineTNC.CompareAndSwap(e, nil)
	engineSetBroadcastFilter(nil)
	C.mercury_host_tnc_release()
	e.sync.Store(false)
	e.say("Disconnected from the modem.")
}

// say logs a line to the window, never waiting on it: after a disconnect
// no one may be reading.
func (e *engineTNC) say(s string) {
	select {
	case e.log <- s:
	default:
	}
}

// liveEngineTNC is the engineTNC holding the TNC, if any.
var liveEngineTNC atomic.Pointer[engineTNC]

// engineTNCShutdown gives the TNC back before the engine stops: its ring is
// freed then, and watch would go on reading it.
func engineTNCShutdown() {
	if e := liveEngineTNC.Load(); e != nil {
		e.Disconnect()
	}
}

// engineTNCHeld says whether the chat window holds this UI's engine's TNC.
func engineTNCHeld() bool {
	e := liveEngineTNC.Load()
	return e != nil && e.IsConnected()
}

func (e *engineTNC) IsConnected() bool {
	e.mu.Lock()
	up := e.done != nil
	e.mu.Unlock()
	return up && !e.evicted.Load()
}

func (e *engineTNC) IsARQConnected() bool { return e.sync.Load() }

// ConnectARQWith places the call and waits, as client.Client does, for the
// session to come up or the call to end.
func (e *engineTNC) ConnectARQWith(src, dst string) error {
	if err := e.command(fmt.Sprintf("CONNECT %s %s", src, dst)); err != nil {
		return err
	}
	tick := time.NewTicker(engineTNCPoll)
	defer tick.Stop()
	timeout := time.After(120 * time.Second)
	notStarted := time.After(15 * time.Second)
	started := false
	ticks := 0
	for {
		select {
		case <-tick.C:
		case <-timeout:
			return fmt.Errorf("connect timeout waiting for CONNECTED")
		case <-notStarted:
			if !started && !e.sync.Load() {
				return fmt.Errorf("the modem did not start the call")
			}
			continue
		}
		if !e.IsConnected() {
			return fmt.Errorf("disconnected")
		}
		if e.sync.Load() {
			return nil
		}
		switch r := engineBusyReason(); {
		case r == "arq":
			started = true
		case started:
			return fmt.Errorf("disconnected before connection established")
		case r == "broadcast" && ticks >= 4:
			// The call would be "arq", which comes first: a second on, the
			// broadcast holding the modem means the call did not start.
			return fmt.Errorf("the modem is sending a broadcast: the call did not start")
		}
		ticks++
	}
}

func (e *engineTNC) DisconnectARQ() error { return e.command("DISCONNECT") }
func (e *engineTNC) AbortARQ() error      { return e.command("ABORT") }

func (e *engineTNC) SendARQMessage(msg string) error {
	plane := C.CString("arq")
	defer C.free(unsafe.Pointer(plane))
	text := C.CString(msg)
	defer C.free(unsafe.Pointer(text))
	switch C.mercury_host_chat_send(plane, text) {
	case C.MERCURY_HOST_OK:
		e.arqChat <- client.ChatMessage{Call: e.cfg.MyCallsign, Text: msg, Time: time.Now()}
		return nil
	case C.MERCURY_HOST_NO_LINK:
		return fmt.Errorf("no ARQ session")
	case C.MERCURY_HOST_FULL:
		return fmt.Errorf("the session's queue is full: try again")
	default:
		return fmt.Errorf("the modem refused the message")
	}
}

func (e *engineTNC) SetBandwidth(hz int) error {
	if err := e.command(fmt.Sprintf("BW%d", hz)); err != nil {
		return err
	}
	e.cfg.BandwidthHz = hz
	return nil
}

func (e *engineTNC) SendCQFrame() error {
	if err := e.command(fmt.Sprintf("CQFRAME %s %d", e.cfg.MyCallsign, e.cfg.BandwidthHz)); err != nil {
		return err
	}
	e.log <- fmt.Sprintf("CQ Frame TX: %s (%d Hz)", e.cfg.MyCallsign, e.cfg.BandwidthHz)
	return nil
}

// SendBroadcast sends a broadcast chat line with the callsign before it, as
// client.Client does.
func (e *engineTNC) SendBroadcast(msg string) error {
	return engineSendBroadcastChat(e.cfg.MyCallsign + ": " + msg)
}

func (e *engineTNC) SetBroadcastFrameFilter(f client.BroadcastFrameFilter) {
	engineSetBroadcastFilter(f)
}

// watch follows the session for the window, as client.Client's readers do
// with the TNC's lines and the data port: the session's received bytes as chat
// lines (read so that its ring never fills), the state changes from the
// telemetry, and the broadcast chat lines the engine stores.
func (e *engineTNC) watch(done chan struct{}, seq uint64) {
	tick := time.NewTicker(engineTNCPoll)
	defer tick.Stop()
	var buf [4096]byte
	var rx client.LineAssembler
	var lastRX time.Time
	wasTX := false
	send := func(ch chan string, s string) bool {
		select {
		case ch <- s:
			return true
		case <-done:
			return false
		}
	}
	chat := func(ch chan client.ChatMessage, m client.ChatMessage) bool {
		select {
		case ch <- m:
			return true
		case <-done:
			return false
		}
	}
	peer := func(t telemetryState) string {
		if t.DestCallsign != "" {
			return t.DestCallsign
		}
		return e.cfg.TargetCallsign
	}
	for {
		select {
		case <-done:
			return
		case <-tick.C:
		}
		var t telemetryState
		if currentTelemetry != nil {
			t = currentTelemetry()
		}
		if C.mercury_host_tnc_held() == 0 {
			e.evicted.Store(true)
		}
		for {
			n := int(C.mercury_host_arq_read((*C.uint8_t)(unsafe.Pointer(&buf[0])), C.size_t(len(buf))))
			if n == 0 {
				break
			}
			lastRX = time.Now()
			for _, line := range rx.Push(buf[:n]) {
				if !chat(e.arqChat, client.ChatMessage{Call: peer(t), Text: line, Time: lastRX}) {
					return
				}
			}
		}
		// The session's last line may have no ending: show it once the link
		// has been quiet a while, or when the session ends.
		if up := t.Sync; up != e.sync.Load() || (rx.Pending() && time.Since(lastRX) > client.ChatPartialFlush) {
			if line := rx.Flush(); line != "" && !chat(e.arqChat, client.ChatMessage{Call: peer(t), Text: line, Time: time.Now()}) {
				return
			}
		}
		if up := t.Sync; up != e.sync.Load() {
			e.sync.Store(up)
			s := "DISCONNECTED"
			if up {
				s = "CONNECTED"
			}
			if !send(e.status, s) {
				return
			}
		}
		tx := t.Direction == "tx"
		if wasTX && !tx && !send(e.status, "PTT OFF") {
			return
		}
		wasTX = tx
		if engineChatLastSeq() == seq {
			continue
		}
		var msgs []HistoryMessage
		msgs, seq = engineChatSince(seq)
		for _, m := range msgs {
			if m.Plane != "bcast" {
				continue // ARQ lines come from the session's bytes, above
			}
			cm := client.ChatMessage{Text: m.Text, Broadcast: true,
				Time: time.Unix(m.Ts, int64(m.Ms)*int64(time.Millisecond))}
			if !chat(e.bcastChat, cm) {
				return
			}
		}
	}
}

// engineBusyReason is what holds the modem: "arq" (a session up or being
// placed), "broadcast", or "" when free.
func engineBusyReason() string {
	r := C.mercury_host_busy_reason()
	if r == nil {
		return ""
	}
	return C.GoString(r)
}
