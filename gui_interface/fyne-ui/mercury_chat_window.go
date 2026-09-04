package main

import (
	"fmt"
	"strconv"
	"strings"
	"time"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/layout"
	"fyne.io/fyne/v2/widget"

	"mercury-client/client"
)

// Ring-buffer caps so the log and chat panes stay bounded during long
// transfers: each append would otherwise rebuild a growing string / prepend a
// new widget forever, making the window unusable.
const (
	maxLogLines     = 200
	maxChatMessages = 200
)

// mercuryClientSingleton holds the single open chat window.  The engine only
// accepts one control client at a time; opening a second window would evict
// the first and tear down its ARQ session.  Reuse the window instead.
var mercuryClientSingleton *chatWindow

func openMercuryClientWindow(app fyne.App, telemetry telemetryState, arqPort, broadcastPort int) {
	if mercuryClientSingleton != nil {
		mercuryClientSingleton.win.RequestFocus()
		return
	}
	cw := &chatWindow{}
	cw.build(app, telemetry, arqPort, broadcastPort)
	mercuryClientSingleton = cw
}

type chatWindow struct {
	win  fyne.Window
	mc   *client.Client
	done chan struct{}
	log  *widget.Entry
	// logLines is the bounded ring (newest first) backing the log Entry,
	// so appending never re-splits the widget's own text.
	logLines []string

	arqBox      *fyne.Container
	arqScroll   *container.Scroll
	bcastBox    *fyne.Container
	bcastScroll *container.Scroll

	myCall    *widget.Entry
	target    *widget.Entry
	ip        *widget.Entry
	arqPort   *widget.Entry
	bcastPort *widget.Entry
	bandwidth *widget.Select

	connectBtn    *widget.Button
	disconnectBtn *widget.Button
	arqConnect    *widget.Button
	arqDisconnect *widget.Button
	arqAbort      *widget.Button
	sendARQ       *widget.Button
	sendBcast     *widget.Button
	sendBcastWrap *hoverTooltipButton
	sendCQ        *widget.Button
	bcastFile     *broadcastFilePanel
	arqMsg        *widget.Entry
	bcastMsg      *widget.Entry
	bcastCount    *widget.Label

	// bcastDisabledReason is shown as a hover tooltip on the Broadcast
	// message button while it is disabled (e.g. during an ARQ session).
	bcastDisabledReason string

	// cqSending is set while a CQ frame is being transmitted; broadcast and
	// ARQ connect are blocked until the engine reports the transmission done.
	// Owned by setCQBusy and touched only from the UI thread.
	cqSending bool
}

func (cw *chatWindow) build(app fyne.App, telemetry telemetryState, arqPort, broadcastPort int) {
	cw.win = app.NewWindow("Mercury Client")

	cw.myCall = widget.NewEntry()
	// The limit includes the callsign, so it moves when the callsign does.
	defer func() {
		prev := cw.myCall.OnChanged
		cw.myCall.OnChanged = func(v string) {
			if prev != nil {
				prev(v)
			}
			cw.enforceBroadcastLimit()
		}
	}()
	cw.myCall.SetText(defaultCall(telemetry.UserCallsign, "NOCALL"))
	cw.target = widget.NewEntry()
	cw.target.SetText(defaultCall(telemetry.DestCallsign, "DEST"))
	cw.ip = widget.NewEntry()
	cw.ip.SetText("127.0.0.1")
	cw.arqPort = widget.NewEntry()
	cw.arqPort.SetText(strconv.Itoa(arqPort))
	cw.bcastPort = widget.NewEntry()
	cw.bcastPort.SetText(strconv.Itoa(broadcastPort))

	cw.bandwidth = widget.NewSelect([]string{"2300 Hz", "500 Hz"}, cw.onBandwidthChange)
	cw.bandwidth.SetSelected("2300 Hz")

	cw.arqMsg = widget.NewEntry()
	cw.arqMsg.SetPlaceHolder("Type message to be sent...")
	cw.bcastMsg = widget.NewEntry()
	cw.bcastMsg.SetPlaceHolder("Type broadcast message...")
	// A broadcast message that overruns one modem frame is TRUNCATED by the
	// TNC -- the operator just sees their last characters vanish.  The limit
	// depends on the mode and the callsign, so show it and enforce it here
	// rather than let it be discovered on the air.  See issue #243.
	cw.bcastMsg.OnChanged = func(string) { cw.enforceBroadcastLimit() }
	cw.bcastCount = widget.NewLabel("")

	cw.log = widget.NewMultiLineEntry()
	cw.log.SetPlaceHolder("Activity log...")
	cw.log.Wrapping = fyne.TextWrapBreak
	cw.log.Disable()

	cw.arqBox = container.NewVBox()
	cw.arqScroll = container.NewScroll(cw.arqBox)
	cw.bcastBox = container.NewVBox()
	cw.bcastScroll = container.NewScroll(cw.bcastBox)

	cw.connectBtn = widget.NewButton("Connect modem", cw.onConnect)
	cw.disconnectBtn = widget.NewButton("Disconnect modem", cw.onDisconnect)
	cw.disconnectBtn.Disable()
	cw.arqConnect = widget.NewButton("Connect ARQ", cw.onARQConnect)
	cw.arqConnect.Disable()
	cw.arqDisconnect = widget.NewButton("Disconnect ARQ", cw.onARQDisconnect)
	cw.arqDisconnect.Disable()
	cw.arqAbort = widget.NewButton("Abort", func() {
		if mc := cw.mc; mc != nil {
			mc.AbortARQ()
		}
	})
	cw.arqAbort.Disable()
	cw.sendARQ = widget.NewButton("Send message", cw.onSendARQ)
	cw.sendARQ.Disable()
	cw.sendBcast = widget.NewButton("Broadcast message", cw.onSendBroadcast)
	cw.sendBcastWrap = newHoverTooltipButton(cw.sendBcast, cw.win.Canvas(), func() string {
		return cw.bcastDisabledReason
	})
	cw.sendBcastWrap.Disable()
	cw.sendCQ = widget.NewButton("Send CQ Frame", cw.onSendCQ)
	cw.sendCQ.Disable()

	cfgForm := widget.NewForm(
		&widget.FormItem{Text: "My Callsign", Widget: cw.myCall},
		&widget.FormItem{Text: "Target Callsign", Widget: cw.target},
		&widget.FormItem{Text: "IP/Host", Widget: cw.ip},
		&widget.FormItem{Text: "ARQ Port", Widget: cw.arqPort},
		&widget.FormItem{Text: "Broadcast Port", Widget: cw.bcastPort},
		&widget.FormItem{Text: "Bandwidth", Widget: cw.bandwidth},
	)

	modemRow := container.NewHBox(cw.connectBtn, cw.disconnectBtn)
	sessionRow := container.NewGridWithColumns(4,
		cw.arqConnect,
		cw.arqDisconnect,
		cw.arqAbort,
		cw.sendCQ,
	)

	// File broadcast rides the same broadcast socket the chat below it uses,
	// so it lives with the broadcast controls and is enabled by the same signal.
	cw.bcastFile = newBroadcastFilePanel(cw.win, fyne.CurrentApp().Preferences(),
		func() broadcastSender {
			if cw.mc == nil {
				return nil
			}
			return cw.mc
		},
		func(f func([]byte) bool) {
			if cw.mc == nil {
				return
			}
			if f == nil {
				cw.mc.SetBroadcastFrameFilter(nil)
				return
			}
			cw.mc.SetBroadcastFrameFilter(client.BroadcastFrameFilter(f))
		},
		cw.logMsg)

	controls := container.NewVBox(
		cfgForm,
		modemRow,
		widget.NewLabel("Session"), sessionRow,
		widget.NewSeparator(),
		cw.arqMsg, cw.sendARQ,
		widget.NewSeparator(),
		cw.bcastMsg, cw.bcastCount, cw.sendBcastWrap,
		widget.NewSeparator(),
		cw.bcastFile.content(),
	)

	left := container.NewVBox(controls, layout.NewSpacer())

	arqChatBox := container.NewBorder(
		widget.NewLabelWithStyle("Chat messages", fyne.TextAlignCenter, fyne.TextStyle{Bold: true}),
		nil, nil, nil, cw.arqScroll,
	)
	bcastChatBox := container.NewBorder(
		widget.NewLabelWithStyle("Broadcast Messages", fyne.TextAlignCenter, fyne.TextStyle{Bold: true}),
		nil, nil, nil, cw.bcastScroll,
	)
	logBox := container.NewBorder(
		widget.NewLabelWithStyle("Activity Log", fyne.TextAlignCenter, fyne.TextStyle{Bold: true}),
		nil, nil, nil, container.NewScroll(cw.log),
	)

	right := container.NewBorder(nil, nil, nil, nil,
		container.NewVSplit(
			container.NewVSplit(bcastChatBox, arqChatBox),
			logBox,
		),
	)

	cw.win.SetContent(container.NewHSplit(left, right))
	cw.win.Resize(fyne.NewSize(800, 600))
	cw.win.SetOnClosed(func() {
		if cw.bcastFile != nil {
			cw.bcastFile.stopForShutdown()
		}
		if cw.mc != nil {
			cw.mc.Disconnect()
		}
		if cw.done != nil {
			close(cw.done)
		}
		if mercuryClientSingleton == cw {
			mercuryClientSingleton = nil
		}
	})
	cw.win.Show()
}

// broadcastChatLimit is how many characters will actually reach the air, given
// the mode the engine is running and the callsign in the form.  0 means the
// limit is unknown (no engine) and no cap is applied.
func (cw *chatWindow) broadcastChatLimit() int {
	mode := broadcastEngineMode()
	if mode < 0 {
		return 0
	}
	fs := broadcastModeFrameSize(mode)
	if fs <= 0 {
		return 0
	}
	return client.BroadcastChatLimit(fs, strings.TrimSpace(cw.myCall.Text))
}

// enforceBroadcastLimit caps the entry at what will actually be transmitted and
// shows the operator how much room is left.  Trimming as they type is blunt,
// but the alternative is letting them compose a message the TNC will quietly
// shorten.
func (cw *chatWindow) enforceBroadcastLimit() {
	limit := cw.broadcastChatLimit()
	if limit <= 0 {
		cw.bcastCount.SetText("")
		return
	}
	if len(cw.bcastMsg.Text) > limit {
		cw.bcastMsg.SetText(cw.bcastMsg.Text[:limit])
		return // SetText re-enters; the count is updated on that pass
	}
	cw.bcastCount.SetText(fmt.Sprintf("%d/%d characters this mode",
		len(cw.bcastMsg.Text), limit))
}

func (cw *chatWindow) logMsg(format string, args ...any) {
	fyne.Do(func() {
		ts := time.Now().Format("15:04:05")
		line := fmt.Sprintf("[%s] "+format, append([]any{ts}, args...)...)
		cw.logLines = append([]string{line}, cw.logLines...)
		if len(cw.logLines) > maxLogLines {
			cw.logLines = cw.logLines[:maxLogLines]
		}
		cw.log.SetText(strings.Join(cw.logLines, "\n"))
		cw.log.Refresh()
	})
}

func (cw *chatWindow) appendRichChat(box *fyne.Container, call, text string) {
	fyne.Do(func() {
		var rt *widget.RichText
		if call != "" {
			rt = widget.NewRichText(
				&widget.TextSegment{
					Text: call + ": ",
					Style: widget.RichTextStyle{
						TextStyle: fyne.TextStyle{Bold: true},
					},
				},
				&widget.TextSegment{
					Text: text,
				},
			)
		} else {
			rt = widget.NewRichText(
				&widget.TextSegment{Text: text},
			)
		}
		rt.Wrapping = fyne.TextWrapBreak
		box.Objects = append([]fyne.CanvasObject{rt}, box.Objects...)
		if len(box.Objects) > maxChatMessages {
			box.Objects = box.Objects[:maxChatMessages]
		}
		box.Refresh()
	})
}

func splitCallText(line string) (call, text string) {
	if idx := strings.Index(line, ": "); idx >= 0 {
		return line[:idx], line[idx+2:]
	}
	return "", line
}

func (cw *chatWindow) setTCP(on bool) {
	fyne.Do(func() {
		if on {
			cw.connectBtn.Disable()
			cw.disconnectBtn.Enable()
			cw.arqConnect.Enable()
			cw.sendBcastWrap.Enable()
			cw.sendCQ.Enable()
			if cw.bcastFile != nil {
				cw.bcastFile.setConnected(true)
			}
		} else {
			cw.connectBtn.Enable()
			cw.disconnectBtn.Disable()
			cw.arqConnect.Disable()
			cw.arqDisconnect.Disable()
			cw.arqAbort.Disable()
			cw.sendARQ.Disable()
			cw.sendBcastWrap.Disable()
			cw.sendCQ.Disable()
			if cw.bcastFile != nil {
				cw.bcastFile.setConnected(false)
			}
		}
	})
}

func (cw *chatWindow) setARQ(on bool) {
	fyne.Do(func() {
		if on {
			cw.arqConnect.Disable()
			cw.arqDisconnect.Enable()
			cw.arqAbort.Enable()
			cw.sendARQ.Enable()
			cw.sendBcastWrap.Disable()
			cw.bcastDisabledReason = "Broadcast is disabled while an ARQ session is active."
			// A CQ is an unsolicited transmission; firing one mid-session puts
			// it on the air on top of the session. The engine will not stop us
			// -- ARQ_CMD_SEND_CQ (arq.c) builds and queues the frame with no
			// conn_state guard -- so the block has to be here.
			cw.sendCQ.Disable()
		} else {
			cw.arqConnect.Enable()
			cw.arqDisconnect.Disable()
			cw.arqAbort.Disable()
			cw.sendARQ.Disable()
			cw.bcastDisabledReason = ""
			if cw.mc != nil && cw.mc.IsConnected() {
				cw.sendBcastWrap.Enable()
				// Not while a CQ of our own is still on the air: setCQBusy
				// owns that case and re-enables when PTT drops.
				if !cw.cqSending {
					cw.sendCQ.Enable()
				}
			}
		}
	})
}

func (cw *chatWindow) onConnect() {
	// Synchronous guard: a double-tap (or key-repeat on a focused button)
	// can fire this twice in one poll batch before setTCP's fyne.Do runs.
	cw.connectBtn.Disable()

	// Disconnect any existing client before opening a new one, so a stale
	// control client is not left open to be evicted by the new connection.
	if cw.mc != nil {
		cw.mc.Disconnect()
		cw.mc = nil
	}
	if cw.done != nil {
		close(cw.done)
		cw.done = nil
	}

	arqPort, err := strconv.Atoi(cw.arqPort.Text)
	if err != nil {
		dialog.ShowError(fmt.Errorf("invalid ARQ port: %v", err), cw.win)
		cw.connectBtn.Enable()
		return
	}
	bcastPort, err := strconv.Atoi(cw.bcastPort.Text)
	if err != nil {
		dialog.ShowError(fmt.Errorf("invalid broadcast port: %v", err), cw.win)
		cw.connectBtn.Enable()
		return
	}
	cfg := client.Config{
		MyCallsign:     cw.myCall.Text,
		TargetCallsign: cw.target.Text,
		IP:             cw.ip.Text,
		ARQPort:        arqPort,
		BroadcastPort:  bcastPort,
		BandwidthHz:    bandwidthFromLabel(cw.bandwidth.Selected),
	}
	mc := client.New(cfg)
	if err := mc.Connect(); err != nil {
		dialog.ShowError(err, cw.win)
		cw.logMsg("connect: %v", err)
		cw.connectBtn.Enable()
		return
	}
	cw.mc = mc
	cw.cqSending = false
	cw.setTCP(true)
	cw.done = make(chan struct{})

	go cw.forwardLog()
	go cw.forwardARQChat()
	go cw.forwardBroadcastChat()
	go cw.forwardStatus()
	go cw.loadHistory(mc)
}

// loadHistory pulls the persisted ARQ/broadcast chat history from the engine
// and re-populates the chat panes, so messages survive an app restart.
func (cw *chatWindow) loadHistory(mc *client.Client) {
	msgs, err := mc.History()
	if err != nil {
		cw.logMsg("history: %v", err)
		return
	}
	fyne.Do(func() {
		for _, m := range msgs {
			if m.Broadcast {
				call, text := splitCallText(m.Text)
				cw.appendRichChat(cw.bcastBox, call, text)
			} else {
				cw.appendRichChat(cw.arqBox, m.Call, m.Text)
			}
		}
	})
	cw.logMsg("Loaded %d messages from history.", len(msgs))
}

func (cw *chatWindow) onDisconnect() {
	mc := cw.mc
	if mc != nil {
		mc.Disconnect()
		cw.mc = nil
	}
	if cw.done != nil {
		close(cw.done)
		cw.done = nil
	}
	cw.setTCP(false)
	cw.setARQ(false)
	cw.cqSending = false
	cw.logMsg("Disconnected.")
}

func (cw *chatWindow) onARQConnect() {
	mc := cw.mc
	if mc == nil || !mc.IsConnected() {
		return
	}
	src := cw.myCall.Text
	dst := cw.target.Text
	cw.arqConnect.Disable()
	cw.logMsg("Connecting ARQ: %s -> %s", src, dst)
	go func() {
		if err := mc.ConnectARQWith(src, dst); err != nil {
			cw.logMsg("ARQ connect: %v", err)
			// Only re-enable the button if the modem is still up: a
			// disconnect mid-handshake leaves cw.mc nil.
			if cw.mc == mc {
				cw.setARQ(false)
			}
			return
		}
		cw.logMsg("ARQ connected.")
		if cw.mc == mc {
			cw.setARQ(true)
		}
	}()
}

func (cw *chatWindow) onARQDisconnect() {
	if mc := cw.mc; mc != nil {
		mc.DisconnectARQ()
	}
	cw.setARQ(false)
	cw.logMsg("ARQ disconnected.")
}

func (cw *chatWindow) onSendARQ() {
	mc := cw.mc
	if mc == nil || !mc.IsConnected() {
		return
	}
	msg := strings.TrimSpace(cw.arqMsg.Text)
	if msg == "" {
		return
	}
	if err := mc.SendARQMessage(msg); err != nil {
		dialog.ShowError(err, cw.win)
		return
	}
	cw.arqMsg.SetText("")
}

func (cw *chatWindow) onBandwidthChange(sel string) {
	hz := bandwidthFromLabel(sel)
	if hz == 0 {
		return
	}
	if mc := cw.mc; mc != nil && mc.IsConnected() {
		if err := mc.SetBandwidth(hz); err != nil {
			cw.logMsg("set bandwidth: %v", err)
		} else {
			cw.logMsg("Bandwidth set to %d Hz.", hz)
		}
	}
}

func (cw *chatWindow) onSendCQ() {
	mc := cw.mc
	if mc == nil || !mc.IsConnected() || cw.cqSending {
		return
	}
	if err := mc.SendCQFrame(); err != nil {
		dialog.ShowError(err, cw.win)
		return
	}
	cw.sendCQ.Disable()
	cw.setCQBusy(true)
	cw.logMsg("CQ frame sent.")
}

// setCQBusy disables broadcast and ARQ connect while a CQ frame is on the air
// and restores them once the engine reports the transmission finished. It owns
// the cqSending flag and every widget it touches, all inside fyne.Do, so the
// flag is only ever accessed from the UI thread.
func (cw *chatWindow) setCQBusy(on bool) {
	fyne.Do(func() {
		if on {
			cw.cqSending = true
			cw.sendCQ.Disable()
			cw.arqConnect.Disable()
			cw.sendBcastWrap.Disable()
			cw.bcastDisabledReason = "CQ frame transmission in progress."
			return
		}
		if !cw.cqSending {
			return
		}
		cw.cqSending = false
		// sendCQ is re-enabled under the same session test as the rest: a
		// session can come up while our CQ is still on the air, and enabling
		// it unconditionally here would undo setARQ's block.
		if cw.mc != nil && cw.mc.IsConnected() && !cw.mc.IsARQConnected() {
			cw.sendCQ.Enable()
			cw.arqConnect.Enable()
			cw.sendBcastWrap.Enable()
			cw.bcastDisabledReason = ""
		}
	})
}

func (cw *chatWindow) onSendBroadcast() {
	mc := cw.mc
	if mc == nil || !mc.IsConnected() {
		return
	}
	msg := strings.TrimSpace(cw.bcastMsg.Text)
	if msg == "" {
		return
	}
	if err := mc.SendBroadcast(msg); err != nil {
		dialog.ShowError(err, cw.win)
		return
	}
	cw.bcastMsg.SetText("")
}

func (cw *chatWindow) forwardLog() {
	mc := cw.mc
	done := cw.done
	if mc == nil || done == nil {
		return
	}
	for {
		select {
		case m, ok := <-mc.LogCh:
			if !ok {
				return
			}
			cw.logMsg("%s", m)
		case <-done:
			return
		}
	}
}

func (cw *chatWindow) forwardARQChat() {
	mc := cw.mc
	done := cw.done
	if mc == nil || done == nil {
		return
	}
	for {
		select {
		case m, ok := <-mc.ARQChatCh:
			if !ok {
				return
			}
			cw.appendRichChat(cw.arqBox, m.Call, m.Text)
		case <-done:
			return
		}
	}
}

func (cw *chatWindow) forwardBroadcastChat() {
	mc := cw.mc
	done := cw.done
	if mc == nil || done == nil {
		return
	}
	for {
		select {
		case m, ok := <-mc.BroadcastChatCh:
			if !ok {
				return
			}
			call, text := splitCallText(m.Text)
			cw.appendRichChat(cw.bcastBox, call, text)
		case <-done:
			return
		}
	}
}

func (cw *chatWindow) forwardStatus() {
	mc := cw.mc
	done := cw.done
	if mc == nil || done == nil {
		return
	}
	for {
		select {
		case s, ok := <-mc.StatusCh:
			if !ok {
				return
			}
			cw.logMsg("TNC Status: %s", s)
			switch s {
			case "CONNECTED":
				cw.setARQ(true)
			case "DISCONNECTED":
				cw.setARQ(false)
			case "PTT OFF":
				// The true end of a CQ transmission: an incoming connection's
				// PENDING/CANCELPENDING never keys the radio, so PTT OFF is the
				// unambiguous "CQ is off the air" signal. setCQBusy owns the
				// cqSending flag and only clears it when one is actually in
				// flight, marshalling the work onto the UI thread.
				cw.setCQBusy(false)
			}
		case <-done:
			return
		}
	}
}

func defaultCall(call, fallback string) string {
	if call != "" {
		return call
	}
	return fallback
}

func bandwidthFromLabel(label string) int {
	switch strings.TrimSpace(label) {
	case "500 Hz":
		return 500
	case "2300 Hz":
		return 2300
	default:
		return 0
	}
}
