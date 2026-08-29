//go:build !mercury_embedded

package main

import (
	"context"
	"fmt"
)

// Without the engine linked in there is nothing to talk to in-process, so the
// UI falls back to the websocket link. This stub keeps the rest of the code
// transport-agnostic instead of sprinkling build tags through it.
type engineLink struct{}

func newEngineLink() *engineLink { return &engineLink{} }

func (l *engineLink) Name() string { return "embedded engine (not built in)" }

func (l *engineLink) probe() (bool, error) {
	return false, fmt.Errorf("engine not embedded")
}

func (l *engineLink) Start(ctx context.Context) (<-chan Event, error) {
	return nil, fmt.Errorf("engine not embedded (build with -tags mercury_embedded)")
}

func (l *engineLink) Send(cmd Command) error {
	return fmt.Errorf("engine not embedded")
}

func (l *engineLink) Close() {}

func (l *engineLink) SetWaterfall(enabled bool) {}

func (l *engineLink) TCPPorts() (arqBase, broadcast int) {
	return 8300, 8100
}

func (l *engineLink) Version() (version, gitHash string) {
	return "unknown", "unknown000"
}
