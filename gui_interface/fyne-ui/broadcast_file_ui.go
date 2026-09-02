//go:build mercury_embedded

// The "Broadcast file" panel: pick a file, choose how many times to send it,
// stop when you like.
//
// Broadcast has no return path, so there is no progress from the far side to
// report -- only what WE have sent. The honest thing to show is frames and
// cycles, not a percentage that would be a guess about someone else's decoder.
package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"sync"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/storage"
	"fyne.io/fyne/v2/widget"
)

// broadcastFilePanel is the UI half; broadcastFileTx is the engine half.
type broadcastFilePanel struct {
	mu sync.Mutex
	tx *broadcastFileTx

	path     string
	pathLbl  *widget.Label
	cycles   *widget.Select
	sendBtn  *widget.Button
	stopBtn  *widget.Button
	status   *widget.Label
	modeLbl  *widget.Label
	chooseBt *widget.Button

	// Receive half.  Frames arrive on the chat client's broadcast socket, so
	// the panel installs a filter there rather than opening a second
	// connection: Mercury's broadcast port accepts exactly one client.
	rx        *broadcastFileRx
	rxEnabled *widget.Check
	rxDir     string
	rxDirLbl  *widget.Label
	rxDirBt   *widget.Button
	rxStatus  *widget.Label
	rxList    *widget.Label

	prefs  fyne.Preferences
	win    fyne.Window
	sender func() broadcastSender // resolved at send time: the chat client
	// setFilter installs (or clears, with nil) the raw-frame filter on the
	// client.  Supplied by the chat window, which owns the connection.
	setFilter func(func([]byte) bool)
	logf      func(string, ...any)
}

// cycleChoices maps the visible label to a cycle count; 0 means until stopped.
var cycleChoices = []string{"Once", "5 times", "10 times", "50 times", "Until stopped"}

func cyclesFromChoice(s string) int {
	switch s {
	case "Once":
		return 1
	case "Until stopped":
		return 0
	default:
		n, _ := strconv.Atoi(s[:len(s)-len(" times")])
		return n
	}
}

func newBroadcastFilePanel(win fyne.Window, prefs fyne.Preferences,
	sender func() broadcastSender, setFilter func(func([]byte) bool),
	logf func(string, ...any)) *broadcastFilePanel {

	p := &broadcastFilePanel{win: win, prefs: prefs, sender: sender,
		setFilter: setFilter, logf: logf}

	p.pathLbl = widget.NewLabel("(no file chosen)")
	p.pathLbl.Wrapping = fyne.TextTruncate
	p.status = widget.NewLabel("")

	// The mode is fixed at engine start by -m and cannot be changed at runtime,
	// so it is REPORTED, not offered. Both stations must be on the same one:
	// the broadcast plane has no negotiation.
	p.modeLbl = widget.NewLabel(broadcastModeDescription())

	p.cycles = widget.NewSelect(cycleChoices, nil)
	p.cycles.SetSelected("Until stopped")

	p.chooseBt = widget.NewButton("Choose file...", p.onChoose)
	p.sendBtn = widget.NewButton("Broadcast", p.onSend)
	p.stopBtn = widget.NewButton("Stop", p.onStop)
	p.sendBtn.Disable()
	p.stopBtn.Disable()

	// The folder the operator set last time, else Downloads, so the common case
	// needs no configuration and a configured station needs none ever again.
	p.rxDir = defaultBroadcastRxDir()
	if prefs != nil {
		p.rxDir = prefs.StringWithFallback(broadcastRxDirPrefKey, p.rxDir)
	}
	p.rxDirLbl = widget.NewLabel(p.rxDir)
	p.rxDirLbl.Wrapping = fyne.TextTruncate
	p.rxDirBt = widget.NewButton("Folder...", p.onChooseRxDir)
	p.rxStatus = widget.NewLabel("")
	p.rxList = widget.NewLabel("")
	p.rxList.Wrapping = fyne.TextWrapWord
	p.rxEnabled = widget.NewCheck("Receive broadcast files", p.onToggleReceive)

	// Restore the operator's choice.  Set the widget without firing OnChanged:
	// receiving cannot start until there is a connection, so the panel arms
	// itself here and setConnected() starts it when the socket comes up.
	if prefs != nil && prefs.BoolWithFallback(broadcastRxOnPrefKey, false) {
		p.rxEnabled.Checked = true
	}

	return p
}

// defaultBroadcastRxDir picks somewhere sensible to put received files.
func defaultBroadcastRxDir() string {
	if home, err := os.UserHomeDir(); err == nil {
		dl := filepath.Join(home, "Downloads")
		if st, err := os.Stat(dl); err == nil && st.IsDir() {
			return dl
		}
		return home
	}
	return "."
}

// content builds the panel. Kept separate so the widgets exist before layout.
func (p *broadcastFilePanel) content() fyne.CanvasObject {
	return container.NewVBox(
		widget.NewLabel("Broadcast file"),
		p.modeLbl,
		container.NewBorder(nil, nil, p.chooseBt, nil, p.pathLbl),
		container.NewGridWithColumns(2, p.cycles, container.NewGridWithColumns(2, p.sendBtn, p.stopBtn)),
		p.status,
		widget.NewSeparator(),
		p.rxEnabled,
		container.NewBorder(nil, nil, p.rxDirBt, nil, p.rxDirLbl),
		p.rxStatus,
		p.rxList,
	)
}

// setConnected enables the panel only while the broadcast socket is up: the
// frames go out over the chat client's connection.
func (p *broadcastFilePanel) setConnected(on bool) {
	fyne.Do(func() {
		p.mu.Lock()
		running := p.tx != nil
		p.mu.Unlock()
		if on && !running && p.path != "" {
			p.sendBtn.Enable()
		} else {
			p.sendBtn.Disable()
		}
		p.chooseBt.Enable()
		if !on {
			p.stopBtn.Disable()
		}
	})

	// Receiving needs the broadcast socket, which belongs to the connection.
	// A station left with the box ticked therefore resumes on its own.
	if on {
		if p.rxEnabled.Checked {
			p.startReceiving()
		}
	} else {
		p.stopReceiving()
	}
}

func (p *broadcastFilePanel) onChooseRxDir() {
	dialog.ShowFolderOpen(func(u fyne.ListableURI, err error) {
		if err != nil || u == nil {
			return
		}
		dir := u.Path()
		if dir == "" {
			dialog.ShowError(fmt.Errorf("that folder is not on the local filesystem"), p.win)
			return
		}
		p.rxDir = dir
		p.rxDirLbl.SetText(dir)
		if p.prefs != nil {
			p.prefs.SetString(broadcastRxDirPrefKey, dir)
		}
		// A running receiver holds the old directory; restart it on the new one.
		if p.rxEnabled.Checked {
			p.stopReceiving()
			p.startReceiving()
		}
	}, p.win)
}

func (p *broadcastFilePanel) onToggleReceive(on bool) {
	if p.prefs != nil {
		p.prefs.SetBool(broadcastRxOnPrefKey, on)
	}
	if on {
		p.startReceiving()
	} else {
		p.stopReceiving()
	}
}

// startReceiving opens a decoder and claims incoming file frames off the chat
// client's broadcast socket.
func (p *broadcastFilePanel) startReceiving() {
	mode := broadcastEngineMode()
	if mode < 0 {
		dialog.ShowError(fmt.Errorf(
			"the modem is running a mode that cannot carry broadcast; restart mercury with -m"), p.win)
		fyne.Do(func() { p.rxEnabled.SetChecked(false) })
		return
	}
	rx, err := newBroadcastFileRx(mode, p.rxDir)
	if err != nil {
		dialog.ShowError(err, p.win)
		fyne.Do(func() { p.rxEnabled.SetChecked(false) })
		return
	}

	p.mu.Lock()
	old := p.rx
	p.rx = rx
	p.mu.Unlock()
	if old != nil {
		old.Close()
	}

	p.rxStatus.SetText("listening for files...")
	p.logf("Receiving broadcast files into %s (mode %d)", p.rxDir, mode)
	p.installFilter()
}

func (p *broadcastFilePanel) stopReceiving() {
	p.mu.Lock()
	rx := p.rx
	p.rx = nil
	p.mu.Unlock()
	if rx != nil {
		rx.Close()
	}
	p.rxStatus.SetText("")
	p.installFilter()
}

// onBroadcastFrame is the filter the chat client calls for every raw frame.
// Runs on the client's reader goroutine, so UI work is marshalled.
func (p *broadcastFilePanel) onBroadcastFrame(frame []byte) bool {
	p.mu.Lock()
	rx := p.rx
	p.mu.Unlock()
	if rx == nil {
		return false
	}

	claimed, pr := rx.Frame(frame)
	if !claimed {
		return false // somebody else's traffic: let chat have it
	}

	switch {
	case pr.Err != nil:
		fyne.Do(func() { p.rxStatus.SetText("receive error: " + pr.Err.Error()) })
		p.logf("Broadcast receive error: %v", pr.Err)
	case pr.Name != "":
		fyne.Do(func() {
			p.rxStatus.SetText("received " + pr.Name)
			cur := p.rxList.Text
			if cur != "" {
				cur += "\n"
			}
			p.rxList.SetText(cur + "\u2713 " + pr.Name)
		})
		p.logf("Received broadcast file: %s -> %s", pr.Name, pr.Path)
	default:
		if pr.Symbols%5 == 0 {
			sym, want := pr.Symbols, pr.ExpectBytes
			fyne.Do(func() {
				p.rxStatus.SetText(fmt.Sprintf("receiving: %d frames of ~%d bytes", sym, want))
			})
		}
	}
	return true
}

func (p *broadcastFilePanel) onChoose() {
	dialog.ShowFileOpen(func(rc fyne.URIReadCloser, err error) {
		if err != nil || rc == nil {
			return
		}
		defer rc.Close()
		path := rc.URI().Path()
		if path == "" {
			dialog.ShowError(fmt.Errorf("that file is not on the local filesystem"), p.win)
			return
		}
		// Refuse an oversized file HERE rather than after the operator has
		// started a transmission: the cap is on the bundle, which is the file
		// plus its name, so check the way the encoder will.
		if st, serr := os.Stat(path); serr == nil {
			limit := broadcastFileMaxBytes()
			bundle := st.Size() + int64(len(filepath.Base(path))) + 5
			if bundle > limit {
				dialog.ShowError(fmt.Errorf(
					"%s is %d bytes; with its name that is %d, over the %d byte limit",
					filepath.Base(path), st.Size(), bundle, limit), p.win)
				return
			}
			if st.Size() == 0 {
				dialog.ShowError(fmt.Errorf("%s is empty", filepath.Base(path)), p.win)
				return
			}
		}
		p.path = path
		p.pathLbl.SetText(filepath.Base(path))
		p.sendBtn.Enable()
	}, p.win)
}

func (p *broadcastFilePanel) onSend() {
	if p.path == "" {
		return
	}
	s := p.sender()
	if s == nil {
		dialog.ShowError(fmt.Errorf("connect to the modem first"), p.win)
		return
	}
	mode := broadcastEngineMode()
	if mode < 0 {
		dialog.ShowError(fmt.Errorf(
			"the modem is running a mode that cannot carry broadcast; restart mercury with -m"), p.win)
		return
	}

	cycles := cyclesFromChoice(p.cycles.Selected)
	tx, err := newBroadcastFileTx(p.path, mode, cycles)
	if err != nil {
		dialog.ShowError(err, p.win)
		return
	}

	p.mu.Lock()
	p.tx = tx
	p.mu.Unlock()

	p.sendBtn.Disable()
	p.chooseBt.Disable()
	p.stopBtn.Enable()
	p.logf("Broadcasting %s (mode %d, %s)", filepath.Base(p.path), mode, p.cycles.Selected)

	go tx.Run(s, func(pr broadcastFileProgress) {
		if !pr.Done {
			// Only repaint every few frames: a fast mode can produce these
			// faster than the UI can usefully redraw.
			if pr.FramesSent%5 != 0 {
				return
			}
			fyne.Do(func() {
				p.status.SetText(fmt.Sprintf("sent %d frames, cycle %s",
					pr.FramesSent, cycleText(pr)))
			})
			return
		}
		p.mu.Lock()
		p.tx = nil
		p.mu.Unlock()
		fyne.Do(func() {
			if pr.Err != nil {
				p.status.SetText("stopped: " + pr.Err.Error())
				p.logf("Broadcast failed: %v", pr.Err)
			} else {
				p.status.SetText(fmt.Sprintf("done: %d frames, %s",
					pr.FramesSent, cycleText(pr)))
				p.logf("Broadcast finished: %d frames", pr.FramesSent)
			}
			p.stopBtn.Disable()
			p.chooseBt.Enable()
			p.sendBtn.Enable()
		})
	})
}

func cycleText(pr broadcastFileProgress) string {
	if pr.CyclesTotal == 0 {
		return fmt.Sprintf("%d (until stopped)", pr.CycleNow)
	}
	return fmt.Sprintf("%d/%d", pr.CycleNow, pr.CyclesTotal)
}

func (p *broadcastFilePanel) onStop() {
	p.mu.Lock()
	tx := p.tx
	p.mu.Unlock()
	if tx != nil {
		tx.Stop() // takes effect at the next frame boundary
		p.status.SetText("stopping...")
	}
}

// stopForShutdown ends any transfer when the window closes, so the goroutine
// does not go on writing to a socket the window is about to drop.
func (p *broadcastFilePanel) stopForShutdown() {
	p.mu.Lock()
	tx := p.tx
	p.mu.Unlock()
	if tx != nil {
		tx.Stop()
	}
	p.stopReceiving()
}

// installFilter wires onBroadcastFrame into the client while a receiver exists,
// and clears it otherwise so chat is not filtered for nothing.
func (p *broadcastFilePanel) installFilter() {
	if p.setFilter == nil {
		return
	}
	p.mu.Lock()
	active := p.rx != nil
	p.mu.Unlock()
	if active {
		p.setFilter(p.onBroadcastFrame)
	} else {
		p.setFilter(nil)
	}
}

// broadcastModeDescription tells the operator, in words they can act on, which
// mode the far station has to be set to.
//
// The index alone is meaningless to anyone who has not read the source, so lead
// with the name, then the speed/robustness trade, then the -m index they would
// actually type.  The mode is fixed at engine start and cannot be changed here.
func broadcastModeDescription() string {
	m := broadcastEngineMode()
	if m < 0 {
		return "Mode: this modem's mode cannot carry broadcast.\n" +
			"Restart mercury with -m (see 'mercury -l')."
	}
	line := fmt.Sprintf("Mode: %s - %s", broadcastModeName(m), broadcastModeCharacter(m))

	// Bitrate and bandwidth come from the running modem, so they describe what
	// is actually being transmitted.  Both are what an operator checks against
	// their licence conditions and their filter setting.
	if br := broadcastEngineBitrate(); br > 0 {
		line += fmt.Sprintf("\n%d bit/s", br)
		if bw := broadcastEngineBandwidthHz(); bw > 0 {
			line += fmt.Sprintf(", %d Hz bandwidth", bw)
		}
		line += fmt.Sprintf(", %d bytes per frame", broadcastModeFrameSize(m))
	} else {
		line += fmt.Sprintf("\n%d bytes per frame", broadcastModeFrameSize(m))
	}
	line += fmt.Sprintf("\nThe receiving station must use the same mode (mercury -m %d).", m)
	return line
}

// broadcastModeCharacter describes the speed/robustness trade in plain words.
// Grouped by frame size, which is what actually drives it: a big frame carries
// more per transmission but needs a better signal to be decoded at all.
func broadcastModeCharacter(mode int) string {
	// Every frame spends 12 bytes on the header, OTI and tag, so a 14-byte mode
	// carries 2 bytes of payload -- 86% overhead, and over 500 frames for 1 kB.
	// Measured: DATAC13 did not finish a 60-byte file in 200 s on a clean
	// channel. Say so rather than let an operator discover it by waiting.
	if fs := broadcastModeFrameSize(mode); fs > 0 && fs-12 < 8 {
		return "NOT practical for files (only 2 bytes per frame after overhead)"
	}
	switch fs := broadcastModeFrameSize(mode); {
	case fs >= 1000:
		return "fastest, needs a strong signal"
	case fs >= 400:
		return "fast, needs a good signal"
	case fs >= 100:
		return "moderate speed and robustness"
	case fs >= 30:
		return "slow, tolerates a poor signal"
	default:
		return "slowest, for the weakest signals"
	}
}

var _ = storage.NewFileURI // keep the storage import if the dialog API changes
