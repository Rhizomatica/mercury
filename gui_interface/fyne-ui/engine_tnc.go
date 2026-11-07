//go:build mercury_embedded

// The chat window's TNC client for this UI's own engine.
//
// engineTNC does what client.Client does over TCP, in-process: the control
// port's own commands through mercury_host_tnc_command, chat through the
// engine's chat calls, and the session followed from the engine's status.  It
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
	"sync"
	"sync/atomic"
	"time"
	"unsafe"

	"mercury-client/client"
)

// engineTNCPoll is how often the session state and the chat store are read.
const engineTNCPoll = 250 * time.Millisecond

type engineTNC struct {
	cfg client.Config

	log       chan string
	status    chan string
	arqChat   chan client.ChatMessage
	bcastChat chan client.ChatMessage

	mu   sync.Mutex
	done chan struct{}

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
		return fmt.Errorf("the modem refused %q", line)
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
	done := e.done
	e.mu.Unlock()
	e.evicted.Store(false)
	e.sync.Store(false)
	e.log <- "Connected to the modem in this program."
	go e.watch(done, engineChatLastSeq())
	return nil
}

// Disconnect gives the TNC back, as a TCP client's leaving does.
func (e *engineTNC) Disconnect() {
	e.mu.Lock()
	done := e.done
	e.done = nil
	e.mu.Unlock()
	if done == nil {
		return
	}
	close(done)
	C.mercury_host_tnc_release()
	e.sync.Store(false)
	e.log <- "Disconnected from the modem."
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
		if engineBusyReason() == "arq" {
			started = true
		} else if started {
			return fmt.Errorf("disconnected before connection established")
		}
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
// with the TNC's lines: the state changes, the chat lines the engine stores
// (sent and received, on either plane), and the session's received bytes,
// read so that its ring never fills (the lines are in the store).
func (e *engineTNC) watch(done chan struct{}, seq uint64) {
	tick := time.NewTicker(engineTNCPoll)
	defer tick.Stop()
	var cst C.ui_status_t
	var drain [4096]byte
	wasTX := false
	send := func(ch chan string, s string) bool {
		select {
		case ch <- s:
			return true
		case <-done:
			return false
		}
	}
	for {
		select {
		case <-done:
			return
		case <-tick.C:
		}
		for C.mercury_host_arq_read((*C.uint8_t)(unsafe.Pointer(&drain[0])), C.size_t(len(drain))) > 0 {
		}
		if C.mercury_ui_get_status(&cst) != 0 {
			if bool(cst.client_tcp_connected) {
				e.evicted.Store(true)
			}
			if up := bool(cst.sync); up != e.sync.Load() {
				e.sync.Store(up)
				s := "DISCONNECTED"
				if up {
					s = "CONNECTED"
				}
				if !send(e.status, s) {
					return
				}
			}
			tx := bool(cst.transmitting)
			if wasTX && !tx && !send(e.status, "PTT OFF") {
				return
			}
			wasTX = tx
		}
		if engineChatLastSeq() == seq {
			continue
		}
		var msgs []HistoryMessage
		msgs, seq = engineChatSince(seq)
		for _, m := range msgs {
			cm := client.ChatMessage{Text: m.Text, Time: time.Unix(m.Ts, int64(m.Ms)*int64(time.Millisecond))}
			ch := e.arqChat
			if m.Plane == "bcast" {
				cm.Broadcast, ch = true, e.bcastChat
			} else if m.Dir == "tx" {
				cm.Call = e.cfg.MyCallsign
			} else {
				cm.Call = m.Peer
			}
			select {
			case ch <- cm:
			case <-done:
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
