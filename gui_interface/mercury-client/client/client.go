// Package client provides a reusable Mercury modem client. It wraps the
// low-level TCP connections to a Mercury HF modem and exposes a higher-level
// API for ARQ and broadcast sessions, chat messages and status events. The
// package is GUI-agnostic: consumers receive events over channels and decide
// how to render them.
package client

import (
	"encoding/json"
	"fmt"
	"strings"
	"sync"

	"mercury-client/modem"
)

// Config holds the connection parameters for a Mercury modem.
type Config struct {
	MyCallsign     string
	TargetCallsign string
	IP             string
	ARQPort        int
	BroadcastPort  int
	BandwidthHz    int
}

// ChatMessage is a single chat line emitted for display.
type ChatMessage struct {
	Call      string
	Text      string
	Broadcast bool
}

// Client wraps a modem.ModemClient and exposes chat and session events to a
// UI. It is safe for concurrent use.
type Client struct {
	mu    sync.Mutex
	modem *modem.ModemClient
	done  chan struct{}
	cfg   Config

	// LogCh receives human-readable activity lines.
	LogCh chan string
	// ARQChatCh receives complete ARQ chat messages.
	ARQChatCh chan ChatMessage
	// BroadcastChatCh receives complete broadcast chat messages.
	BroadcastChatCh chan ChatMessage
	// StatusCh receives ARQ session status ("CONNECTED", "DISCONNECTED", ...).
	StatusCh chan string

	// bcastFilter, when set, gets first refusal on every raw broadcast frame.
	bcastFilter BroadcastFrameFilter

	remoteCall        string
	chatRxBuffer      string
	broadcastRxBuffer string
}

// New returns a Client configured with the given parameters.
func New(cfg Config) *Client {
	if cfg.IP == "" {
		cfg.IP = "127.0.0.1"
	}
	if cfg.ARQPort == 0 {
		cfg.ARQPort = 8300
	}
	if cfg.BroadcastPort == 0 {
		cfg.BroadcastPort = 8100
	}
	if cfg.BandwidthHz == 0 {
		cfg.BandwidthHz = 2300
	}
	return &Client{
		cfg:             cfg,
		LogCh:           make(chan string, 256),
		ARQChatCh:       make(chan ChatMessage, 256),
		BroadcastChatCh: make(chan ChatMessage, 256),
		StatusCh:        make(chan string, 64),
	}
}

// Connect establishes the TCP connections to the modem and starts the event
// forwarding goroutines. Consumers must start ranging over the event channels
// before or right after calling Connect so buffered events are drained.
func (c *Client) Connect() error {
	arqControlAddr := fmt.Sprintf("%s:%d", c.cfg.IP, c.cfg.ARQPort)
	arqDataAddr := fmt.Sprintf("%s:%d", c.cfg.IP, c.cfg.ARQPort+1)
	broadcastAddr := fmt.Sprintf("%s:%d", c.cfg.IP, c.cfg.BroadcastPort)

	mc := modem.NewModemClient(arqControlAddr, arqDataAddr, broadcastAddr)
	if err := mc.Connect(); err != nil {
		return err
	}

	c.mu.Lock()
	if c.done != nil {
		close(c.done)
	}
	c.done = make(chan struct{})
	c.modem = mc
	c.mu.Unlock()

	c.LogCh <- "Connected to modem."
	mc.SendCommand("MYCALL " + c.cfg.MyCallsign)
	mc.SendCommand("LISTEN ON")
	mc.SendCommand("PUBLIC OFF")
	mc.SendCommand("COMPRESSION OFF")
	mc.SendCommand(fmt.Sprintf("BW%d", c.cfg.BandwidthHz))

	go c.updateLog()
	go c.handleIncomingARQ()
	go c.handleIncomingARQData()
	go c.handleIncomingBroadcast()
	go c.handleStatus()

	return nil
}

// Disconnect closes all connections and stops the event goroutines.
func (c *Client) Disconnect() {
	c.mu.Lock()
	mc := c.modem
	c.modem = nil
	done := c.done
	c.done = nil
	c.mu.Unlock()

	if mc != nil {
		mc.Disconnect()
	}
	if done != nil {
		close(done)
	}
	c.LogCh <- "Disconnected from modem."
}

// BroadcastFrameFilter inspects a raw broadcast frame before it is treated as
// chat text.  Returning true claims the frame: chat never sees it.
//
// The broadcast plane carries whatever anyone puts on it, and Mercury's TNC
// hands every frame to its single client.  Without this, binary file-transfer
// frames would be appended to the chat buffer as mojibake.
type BroadcastFrameFilter func(frame []byte) bool

// SetBroadcastFrameFilter installs (or clears, with nil) the filter.
func (c *Client) SetBroadcastFrameFilter(f BroadcastFrameFilter) {
	c.mu.Lock()
	c.bcastFilter = f
	c.mu.Unlock()
}

// SendBroadcastFrame writes one already-framed modem frame to the broadcast
// port.  SendBroadcast() is for text; a RaptorQ frame is binary and must not be
// touched, so it gets its own path rather than a string round-trip.
func (c *Client) SendBroadcastFrame(frame []byte) error {
	c.mu.Lock()
	mc := c.modem
	c.mu.Unlock()
	if mc == nil {
		return fmt.Errorf("not connected to the broadcast port")
	}
	return mc.SendBroadcastModemFrame(frame)
}

// IsConnected reports whether the TCP links to the modem are up.
func (c *Client) IsConnected() bool {
	c.mu.Lock()
	mc := c.modem
	c.mu.Unlock()
	return mc != nil && mc.IsConnected()
}

// IsARQConnected reports whether an ARQ session is established.
func (c *Client) IsARQConnected() bool {
	c.mu.Lock()
	mc := c.modem
	c.mu.Unlock()
	return mc != nil && mc.IsARQConnected()
}

// RemoteCallsign returns the callsign of the currently connected remote
// station, or "" if no ARQ session is established.
func (c *Client) RemoteCallsign() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.remoteCall
}

// storedMessage is one JSONL history record as written by the engine.
type storedMessage struct {
	Plane string `json:"plane"`
	Dir   string `json:"dir"`
	Peer  string `json:"peer"`
	Text  string `json:"text"`
}

// History fetches the persisted ARQ and broadcast chat history from the modem
// and returns it as chat messages (oldest first). Broadcast messages keep their
// "CALLSIGN: text" payload so the UI can split them the same way it splits live
// traffic; ARQ messages carry the peer callsign (or the local one for TX).
func (c *Client) History() ([]ChatMessage, error) {
	c.mu.Lock()
	mc := c.modem
	c.mu.Unlock()
	if mc == nil || !mc.IsConnected() {
		return nil, fmt.Errorf("not connected to modem")
	}

	lines, err := mc.GetHistory()
	if err != nil {
		return nil, err
	}

	out := make([]ChatMessage, 0, len(lines))
	for _, line := range lines {
		var m storedMessage
		if err := json.Unmarshal([]byte(line), &m); err != nil {
			continue
		}
		switch m.Plane {
		case "arq":
			call := m.Peer
			if m.Dir == "tx" {
				call = c.cfg.MyCallsign
			}
			if strings.TrimSpace(m.Text) == "" {
				continue
			}
			out = append(out, ChatMessage{Call: call, Text: m.Text})
		case "bcast":
			if strings.TrimSpace(m.Text) == "" {
				continue
			}
			out = append(out, ChatMessage{Text: m.Text, Broadcast: true})
		}
	}
	return out, nil
}

// ConnectARQ starts an ARQ session with the configured target callsign. It
// blocks until the session is established or fails, so callers should run it
// in a goroutine.
func (c *Client) ConnectARQ() error {
	return c.ConnectARQWith(c.cfg.MyCallsign, c.cfg.TargetCallsign)
}

// ConnectARQWith starts an ARQ session using explicit source and target
// callsigns, overriding whatever was captured in the config at New() time.
// It blocks until the session is established or fails, so callers should run
// it in a goroutine.
func (c *Client) ConnectARQWith(src, dst string) error {
	c.mu.Lock()
	mc := c.modem
	c.mu.Unlock()
	if mc == nil || !mc.IsConnected() {
		return fmt.Errorf("not connected to modem")
	}
	return mc.ConnectARQ(src, dst)
}

// DisconnectARQ sends a clean DISCONNECT to the remote station.
func (c *Client) DisconnectARQ() error {
	c.mu.Lock()
	mc := c.modem
	c.mu.Unlock()
	if mc == nil {
		return fmt.Errorf("not connected to modem")
	}
	return mc.DisconnectARQ()
}

// AbortARQ sends an ABORT to the remote station.
func (c *Client) AbortARQ() error {
	c.mu.Lock()
	mc := c.modem
	c.mu.Unlock()
	if mc == nil {
		return fmt.Errorf("not connected to modem")
	}
	return mc.AbortARQ()
}

// SendARQMessage sends a single ARQ chat message to the connected remote
// station. A trailing newline is appended so the receiver flushes the line.
func (c *Client) SendARQMessage(msg string) error {
	if strings.TrimSpace(msg) == "" {
		return fmt.Errorf("empty message")
	}
	c.mu.Lock()
	mc := c.modem
	c.mu.Unlock()
	if mc == nil || !mc.IsConnected() {
		return fmt.Errorf("not connected to modem")
	}
	if err := mc.SendARQData([]byte(msg + "\n")); err != nil {
		return err
	}
	c.LogCh <- fmt.Sprintf("ARQ Data TX: %d bytes", len(msg))
	c.ARQChatCh <- ChatMessage{Call: c.cfg.MyCallsign, Text: msg}
	return nil
}

// SendARQFile streams a file over the ARQ data port.
func (c *Client) SendARQFile(filePath string) error {
	c.mu.Lock()
	mc := c.modem
	c.mu.Unlock()
	if mc == nil || !mc.IsConnected() {
		return fmt.Errorf("not connected to modem")
	}
	return mc.SendARQFile(filePath)
}

// SetBandwidth updates the modem's bandwidth and stores it for the next
// connection. Supported values are 500 and 2300 (Hz).
func (c *Client) SetBandwidth(hz int) error {
	if hz != 500 && hz != 2300 {
		return fmt.Errorf("unsupported bandwidth: %d Hz", hz)
	}
	c.mu.Lock()
	mc := c.modem
	c.mu.Unlock()
	if mc == nil || !mc.IsConnected() {
		return fmt.Errorf("not connected to modem")
	}
	if err := mc.SendCommand(fmt.Sprintf("BW%d", hz)); err != nil {
		return err
	}
	c.cfg.BandwidthHz = hz
	return nil
}

// SendCQFrame queues a one-shot CQ frame advertising the local callsign and
// the currently configured bandwidth.
func (c *Client) SendCQFrame() error {
	c.mu.Lock()
	mc := c.modem
	c.mu.Unlock()
	if mc == nil || !mc.IsConnected() {
		return fmt.Errorf("not connected to modem")
	}
	if err := mc.SendCQFrame(c.cfg.MyCallsign, c.cfg.BandwidthHz); err != nil {
		return err
	}
	c.LogCh <- fmt.Sprintf("CQ Frame TX: %s (%d Hz)", c.cfg.MyCallsign, c.cfg.BandwidthHz)
	return nil
}

// SendBroadcast sends a broadcast message. The local callsign is prefixed to
// the payload because the Mercury broadcast plane carries no callsign, so
// receiving stations can attribute the message.
// broadcastChatOverhead is what SendBroadcast adds around the operator's text:
// the callsign, ": " and the terminating newline.  Kept beside the Sprintf that
// produces it so the two cannot drift.
func broadcastChatOverhead(callsign string) int {
	return len(callsign) + len(": ") + len("\n")
}

// BroadcastChatLimit is how many characters of message will actually reach the
// air for a given modem frame size.
//
// Mercury frames a broadcast message with a 1-byte header and a 2-byte length
// prefix, and when the payload already fills the frame there is nowhere to put
// them, so the TNC truncates -- the operator just sees their last characters
// vanish.  The limit therefore depends on the mode AND the callsign, which
// nobody can be expected to work out:
//
//	DATAC3 (126 B) as "PU2UIT-3"  ->  112 characters
//	DATAC4  (54 B) as "PU2UIT-3"  ->   40 characters
//
// Returns 0 if frameSize leaves no room at all, so a caller can tell the
// difference between "short message" and "this mode cannot carry chat".
func BroadcastChatLimit(frameSize int, callsign string) int {
	const mercuryFraming = 3 // header + 2-byte length prefix
	n := frameSize - mercuryFraming - broadcastChatOverhead(callsign)
	if n < 0 {
		return 0
	}
	return n
}

func (c *Client) SendBroadcast(msg string) error {
	if strings.TrimSpace(msg) == "" {
		return fmt.Errorf("empty message")
	}
	c.mu.Lock()
	mc := c.modem
	c.mu.Unlock()
	if mc == nil || !mc.IsConnected() {
		return fmt.Errorf("not connected to modem")
	}
	payload := fmt.Sprintf("%s: %s\n", c.cfg.MyCallsign, msg)
	if err := mc.SendBroadcast([]byte(payload)); err != nil {
		return err
	}
	c.BroadcastChatCh <- ChatMessage{Call: c.cfg.MyCallsign, Text: msg, Broadcast: true}
	return nil
}

// sendOrStop forwards v to ch unless done is closed first.  It returns false
// when the goroutine should stop instead of blocking forever on a full channel
// whose only drainer has already exited.
func sendOrStop[T any](ch chan<- T, v T, done <-chan struct{}) bool {
	select {
	case ch <- v:
		return true
	case <-done:
		return false
	}
}

// updateLog forwards modem log lines to LogCh.
func (c *Client) updateLog() {
	c.mu.Lock()
	mc := c.modem
	done := c.done
	c.mu.Unlock()
	if mc == nil || done == nil {
		return
	}
	for {
		select {
		case logMsg, ok := <-mc.LogCh:
			if !ok {
				return
			}
			if !sendOrStop(c.LogCh, logMsg, done) {
				return
			}
		case <-done:
			return
		}
	}
}

// handleIncomingARQ forwards control lines and tracks the remote callsign.
func (c *Client) handleIncomingARQ() {
	c.mu.Lock()
	mc := c.modem
	done := c.done
	c.mu.Unlock()
	if mc == nil || done == nil {
		return
	}
	for {
		select {
		case arqMsg, ok := <-mc.IncomingARQCh:
			if !ok {
				return
			}
			if !sendOrStop(c.LogCh, fmt.Sprintf("ARQ Control: %s", arqMsg), done) {
				return
			}
			if strings.HasPrefix(arqMsg, "CONNECTED") {
				if !c.updateRemoteCall(arqMsg, done) {
					return
				}
			}
		case <-done:
			return
		}
	}
}

// updateRemoteCall derives the remote callsign from a "CONNECTED <src> <dst> ..."
// line by matching against the local callsign.
func (c *Client) updateRemoteCall(connectedLine string, done <-chan struct{}) bool {
	fields := strings.Fields(connectedLine)
	if len(fields) < 3 {
		return true
	}
	myCall := strings.ToUpper(strings.TrimSpace(c.cfg.MyCallsign))
	callA := fields[1]
	callB := fields[2]
	var call string
	switch {
	case strings.ToUpper(callA) == myCall:
		call = callB
	case strings.ToUpper(callB) == myCall:
		call = callA
	default:
		call = callA
	}
	c.mu.Lock()
	c.remoteCall = call
	c.mu.Unlock()
	return sendOrStop(c.LogCh, fmt.Sprintf("Remote ARQ callsign set to: %s", call), done)
}

// handleIncomingARQData buffers incoming ARQ data and emits complete
// newline-delimited lines as chat messages.
func (c *Client) handleIncomingARQData() {
	c.mu.Lock()
	mc := c.modem
	done := c.done
	c.mu.Unlock()
	if mc == nil || done == nil {
		return
	}
	for {
		select {
		case data, ok := <-mc.IncomingARQDataCh:
			if !ok {
				return
			}
			if !sendOrStop(c.LogCh, fmt.Sprintf("ARQ Data RX: %d bytes: %q", len(data), string(data)), done) {
				return
			}
			c.mu.Lock()
			c.chatRxBuffer += string(data)
			var lines []ChatMessage
			for {
				idx := strings.IndexByte(c.chatRxBuffer, '\n')
				if idx < 0 {
					break
				}
				line := strings.TrimRight(c.chatRxBuffer[:idx], "\r")
				c.chatRxBuffer = c.chatRxBuffer[idx+1:]
				if strings.TrimSpace(line) != "" {
					call := c.remoteCall
					if call == "" {
						call = c.cfg.TargetCallsign
					}
					lines = append(lines, ChatMessage{Call: call, Text: line})
				}
			}
			if len(c.chatRxBuffer) > 65536 {
				c.chatRxBuffer = c.chatRxBuffer[len(c.chatRxBuffer)-4096:]
			}
			c.mu.Unlock()
			for _, msg := range lines {
				if !sendOrStop(c.ARQChatCh, msg, done) {
					return
				}
			}
		case <-done:
			return
		}
	}
}

// handleIncomingBroadcast buffers incoming broadcast payloads and emits
// complete newline-delimited lines as chat messages. Received broadcast lines
// already carry the sender's callsign prefix in the payload.
func (c *Client) handleIncomingBroadcast() {
	c.mu.Lock()
	mc := c.modem
	done := c.done
	c.mu.Unlock()
	if mc == nil || done == nil {
		return
	}
	for {
		select {
		case data, ok := <-mc.IncomingBcastCh:
			if !ok {
				return
			}
			c.mu.Lock()
			filter := c.bcastFilter
			c.mu.Unlock()
			if filter != nil && filter(data) {
				continue // claimed by the file receiver; not chat
			}

			if !sendOrStop(c.LogCh, fmt.Sprintf("Broadcast RX (Decoded): %s", string(data)), done) {
				return
			}
			c.mu.Lock()
			c.broadcastRxBuffer += string(data)
			var lines []ChatMessage
			for {
				idx := strings.IndexByte(c.broadcastRxBuffer, '\n')
				if idx < 0 {
					break
				}
				line := strings.TrimRight(c.broadcastRxBuffer[:idx], "\r")
				c.broadcastRxBuffer = c.broadcastRxBuffer[idx+1:]
				if strings.TrimSpace(line) != "" {
					lines = append(lines, ChatMessage{Text: line, Broadcast: true})
				}
			}
			if len(c.broadcastRxBuffer) > 65536 {
				c.broadcastRxBuffer = c.broadcastRxBuffer[len(c.broadcastRxBuffer)-4096:]
			}
			c.mu.Unlock()
			for _, msg := range lines {
				if !sendOrStop(c.BroadcastChatCh, msg, done) {
					return
				}
			}
		case <-done:
			return
		}
	}
}

// handleStatus forwards ARQ session status and resets chat state on
// disconnect.
func (c *Client) handleStatus() {
	c.mu.Lock()
	mc := c.modem
	done := c.done
	c.mu.Unlock()
	if mc == nil || done == nil {
		return
	}
	for {
		select {
		case status, ok := <-mc.StatusCh:
			if !ok {
				return
			}
			if !sendOrStop(c.LogCh, fmt.Sprintf("TNC Status: %s", status), done) {
				return
			}
			if !sendOrStop(c.StatusCh, status, done) {
				return
			}
			if status == "DISCONNECTED" {
				c.mu.Lock()
				c.remoteCall = ""
				c.chatRxBuffer = ""
				c.broadcastRxBuffer = ""
				c.mu.Unlock()
			}
		case <-done:
			return
		}
	}
}
