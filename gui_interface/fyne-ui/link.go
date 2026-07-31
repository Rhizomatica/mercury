package main

import "context"

// A Link is where the UI gets engine state and where it sends commands. It
// exists so the rest of the UI never knows, or cares, whether Mercury is
// running inside this process or on a Pi at the far end of the shack.
//
// Two implementations:
//
//	engineLink — the engine linked into this binary, read through CGo. No
//	             socket, no serialization; the default for the desktop app.
//	wsLink     — a Mercury elsewhere, over the websocket the web UI uses.
//
// Both deliver the same events, so the UI code below is written once. Adding a
// third transport means implementing this interface, nothing else.
type Link interface {
	// Name is shown in the status line, e.g. "embedded engine" or
	// "ws://192.168.1.50:10000".
	Name() string

	// Start begins producing events and returns the channel they arrive on.
	// The channel is closed when the link goes down or ctx is cancelled.
	// Events must be consumed promptly; a Link may drop stale telemetry
	// rather than block the engine.
	Start(ctx context.Context) (<-chan Event, error)

	// Send delivers a command to the engine. Safe for concurrent use.
	Send(cmd Command) error

	// Close releases the transport. Idempotent.
	Close()
}

// Command is a UI action on its way to the engine — the same four fields the
// websocket protocol carries, so neither transport has to translate.
type Command struct {
	Name   string
	Value  string
	Value2 string
	Value3 string
}

// DeviceKind identifies which selector a device list belongs to.
type DeviceKind int

const (
	DeviceCapture DeviceKind = iota
	DevicePlayback
	DeviceInputChannel
)

// Event is anything a Link can report. The set is closed: a type switch in the
// UI handles every case, so a new event type will not be silently ignored.
type Event interface{ isLinkEvent() }

// StatusEvent carries a full telemetry snapshot (bitrate, SNR, callsigns,
// byte counters, PTT direction). Sent about twice a second.
type StatusEvent struct{ Status telemetryState }

// SpectrumEvent carries one FFT frame for the waterfall, about 20 times a
// second. Bins are dB magnitudes; the UI owns the slice once delivered.
type SpectrumEvent struct {
	Bins       []float32
	SampleRate int
}

// DeviceListEvent repopulates one of the audio selectors.
type DeviceListEvent struct {
	Kind     DeviceKind
	Items    []optionItem
	Selected string
}

// RadioListEvent repopulates the radio selector and its companion fields.
type RadioListEvent struct {
	Items       []optionItem
	Selected    string
	DevicePath  string
	SerialSpeed string
}

// LinkStateEvent reports the transport coming up or going down, with a
// human-readable detail for the log pane.
type LinkStateEvent struct {
	Up     bool
	Detail string
}

// LogEvent is a line for the UI log pane — anything the transport wants to
// say that is not state.
type LogEvent struct{ Text string }

func (StatusEvent) isLinkEvent()     {}
func (SpectrumEvent) isLinkEvent()   {}
func (DeviceListEvent) isLinkEvent() {}
func (RadioListEvent) isLinkEvent()  {}
func (LinkStateEvent) isLinkEvent()  {}
func (LogEvent) isLinkEvent()        {}

// emit posts an event unless the consumer is gone or lagging. Telemetry is
// disposable — a dropped spectrum frame costs one waterfall row, whereas
// blocking here would stall the poller (embedded) or the socket reader
// (remote) behind a busy UI thread.
func emit(ctx context.Context, ch chan<- Event, ev Event) bool {
	select {
	case ch <- ev:
		return true
	case <-ctx.Done():
		return false
	default:
		return true // consumer lagging; drop this one rather than block
	}
}

// openLink chooses the transport for a session. The embedded engine wins when
// it is available and the user has not pointed the UI at another machine, so
// the desktop app talks to its own engine directly and never opens a socket to
// itself. Naming a remote host keeps the websocket path — that is how one
// desktop UI drives a Mercury on a Pi at the antenna.
func openLink(host, port, scheme string) (Link, error) {
	if isLocalHost(host) {
		l := newEngineLink()
		if _, err := l.probe(); err == nil {
			return l, nil
		}
		// No engine in this build: fall through to the websocket, which still
		// reaches a Mercury running as a separate process on this machine.
	}
	return newWSLink(scheme, host, port), nil
}

// isLocalHost reports whether the host field names this machine (or is empty,
// the default), in which case the embedded engine is preferred.
func isLocalHost(host string) bool {
	switch host {
	case "", "localhost", "127.0.0.1", "::1":
		return true
	}
	return false
}
