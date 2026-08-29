package main

import (
	"context"
	"crypto/tls"
	"encoding/json"
	"fmt"
	"net/url"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

// wsLink drives a Mercury reachable over the network — a Pi at the antenna, or
// the engine in this same process when the UI is built without it. It speaks
// the websocket protocol the HERMES web UI uses, and converts what arrives
// into the transport-neutral events in link.go.
type wsLink struct {
	scheme string
	host   string
	port   string

	mu   sync.RWMutex
	conn *websocket.Conn
}

func newWSLink(scheme, host, port string) *wsLink {
	if scheme == "" {
		scheme = "ws"
	}
	if port == "" {
		port = "10000"
	}
	return &wsLink{scheme: scheme, host: host, port: port}
}

func (l *wsLink) Name() string {
	return fmt.Sprintf("%s://%s", l.scheme, netJoinHostPort(l.host, l.port))
}

func (l *wsLink) url() string {
	u := url.URL{Scheme: l.scheme, Host: netJoinHostPort(l.host, l.port), Path: "/websocket"}
	return u.String()
}

func (l *wsLink) Start(ctx context.Context) (<-chan Event, error) {
	dialer := &websocket.Dialer{
		HandshakeTimeout: 5 * time.Second,
		TLSClientConfig:  &tls.Config{InsecureSkipVerify: l.scheme == "wss"},
	}
	conn, _, err := dialer.Dial(l.url(), nil)
	if err != nil {
		return nil, fmt.Errorf("dial %s: %w", l.url(), err)
	}

	l.mu.Lock()
	l.conn = conn
	l.mu.Unlock()

	events := make(chan Event, 64)

	go func() {
		<-ctx.Done()
		_ = conn.Close()
	}()

	go func() {
		defer close(events)
		defer l.Close()

		emit(ctx, events, LinkStateEvent{Up: true, Detail: "Connected to " + l.Name()})

		for {
			msgType, payload, err := conn.ReadMessage()
			if err != nil {
				emit(ctx, events, LinkStateEvent{Up: false, Detail: fmt.Sprintf("read error: %v", err)})
				return
			}
			for _, ev := range decodeWSMessage(msgType, payload) {
				if !emit(ctx, events, ev) {
					return
				}
			}
		}
	}()

	return events, nil
}

func (l *wsLink) Send(cmd Command) error {
	l.mu.RLock()
	conn := l.conn
	l.mu.RUnlock()
	if conn == nil {
		return fmt.Errorf("not connected")
	}

	payload := map[string]any{"command": cmd.Name, "value": cmd.Value}
	if cmd.Value2 != "" {
		payload["value2"] = cmd.Value2
	}
	if cmd.Value3 != "" {
		payload["value3"] = cmd.Value3
	}
	if cmd.Value4 != "" {
		payload["value4"] = cmd.Value4
	}
	if cmd.Value5 != "" {
		payload["value5"] = cmd.Value5
	}
	if cmd.Value6 != "" {
		payload["value6"] = cmd.Value6
	}
	if cmd.Value7 != "" {
		payload["value7"] = cmd.Value7
	}
	return conn.WriteJSON(payload)
}

func (l *wsLink) Close() {
	l.mu.Lock()
	conn := l.conn
	l.conn = nil
	l.mu.Unlock()
	if conn != nil {
		_ = conn.Close()
	}
}

// decodeWSMessage turns one websocket frame into zero or more link events.
// Kept free of connection state so it can be exercised on its own.
func decodeWSMessage(msgType int, payload []byte) []Event {
	switch msgType {
	case websocket.BinaryMessage:
		bins, rate, err := parseSpectrumFrame(payload)
		if err != nil {
			return nil
		}
		return []Event{SpectrumEvent{Bins: bins, SampleRate: rate}}

	case websocket.TextMessage:
		var raw map[string]any
		if err := json.Unmarshal(payload, &raw); err != nil {
			return []Event{LogEvent{Text: fmt.Sprintf("[Raw WS Msg]: %s\n", string(payload))}}
		}

		switch raw["type"] {
		case "status":
			status, err := parseStatusMessage(payload)
			if err != nil {
				return []Event{LogEvent{Text: fmt.Sprintf("Failed to parse status: %v\n", err)}}
			}
			return []Event{StatusEvent{Status: status}}

		case "capture_dev_list":
			return []Event{DeviceListEvent{
				Kind:     DeviceCapture,
				Items:    parseMenuItems(payload),
				Selected: selectedValue(raw, "selected"),
			}}

		case "playback_dev_list":
			return []Event{DeviceListEvent{
				Kind:     DevicePlayback,
				Items:    parseMenuItems(payload),
				Selected: selectedValue(raw, "selected"),
			}}

		case "input_channel":
			return []Event{DeviceListEvent{
				Kind:     DeviceInputChannel,
				Items:    parseChannelItems(payload),
				Selected: selectedValue(raw, "selected"),
			}}

		case "radio_list":
			items := parseMenuItems(payload)
			sortRadioItems(items)
			selected := selectedValue(raw, "selected")
			method, _ := raw["ptt_method"].(string)
			if method == "" {
				if selected != "" && selected != "-1" {
					method = "hamlib" // Backward-compatible old server inference.
				} else {
					method = "none"
				}
			}
			return []Event{RadioListEvent{
				Items:       items,
				Selected:    selected,
				DevicePath:  fmt.Sprint(raw["device_path"]),
				SerialSpeed: fmt.Sprint(raw["serial_speed"]),
				PTTMethod:   method,
				PTTLine:     stringValue(raw, "ptt_line", "rts"),
				PTTInvert:   stringValue(raw, "ptt_invert", "none"),
				CM108GPIO:   stringValue(raw, "cm108_gpio", "3"),
			}}
		}
		return []Event{LogEvent{Text: fmt.Sprintf("[Raw WS Msg]: %s\n", string(payload))}}
	}
	return nil
}

func stringValue(raw map[string]any, key, fallback string) string {
	if value, ok := raw[key]; ok && value != nil {
		return fmt.Sprint(value)
	}
	return fallback
}

// sortRadioItems puts "None" first and the rest alphabetically — hamlib's own
// order is by model id, which is meaningless to a user hunting for their rig.
func sortRadioItems(items []optionItem) {
	sort.Slice(items, func(i, j int) bool {
		if items[i].Name == "None" {
			return true
		}
		if items[j].Name == "None" {
			return false
		}
		return strings.ToLower(items[i].Name) < strings.ToLower(items[j].Name)
	})
}
