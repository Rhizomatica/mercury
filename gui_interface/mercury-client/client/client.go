// Package client provides a reusable Mercury modem client. It wraps the
// low-level TCP connections to a Mercury HF modem and exposes a higher-level
// API for ARQ and broadcast sessions, chat messages and status events. The
// package is GUI-agnostic: consumers receive events over channels and decide
// how to render them.
package client

import (
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
	mc.SendCommand("BW2750")

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

// SendBroadcast sends a broadcast message. The local callsign is prefixed to
// the payload because the Mercury broadcast plane carries no callsign, so
// receiving stations can attribute the message.
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
