//go:build !mercury_embedded

package main

import (
	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/widget"
)

// Broadcast files are cut into frames by the engine (RaptorQ, bcast_file.c),
// so a GUI built without it has no file panel; chat goes uncapped, as with an
// unusable broadcast mode.

type broadcastSender interface {
	SendBroadcastFrame(frame []byte) error
}

type broadcastFilePanel struct{}

func newBroadcastFilePanel(win fyne.Window, prefs fyne.Preferences,
	sender func() broadcastSender, setFilter func(func([]byte) bool),
	logf func(string, ...any)) *broadcastFilePanel {
	return &broadcastFilePanel{}
}

func (p *broadcastFilePanel) content() fyne.CanvasObject {
	return widget.NewLabel("Broadcast files need the embedded engine (build with -tags mercury_embedded).")
}

func (p *broadcastFilePanel) setConnected(on bool) {}

func (p *broadcastFilePanel) stopForShutdown() {}

func broadcastEngineModeRaw() int { return -1 }

func broadcastModeFrameSize(mode int) int { return 0 }
