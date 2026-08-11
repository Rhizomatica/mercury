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

// openMercuryClientWindow creates and shows a chat window backed by the
// vendored mercury-client library.  The engine's telemetry is used to
// pre-fill the callsign fields.
func openMercuryClientWindow(app fyne.App, telemetry telemetryState) {
	cw := &chatWindow{}
	cw.build(app, telemetry)
}

type chatWindow struct {
	win   fyne.Window
	mc    *client.Client
	log   *widget.Entry
	arq   *widget.Entry
	bcast *widget.Entry

	myCall    *widget.Entry
	target    *widget.Entry
	ip        *widget.Entry
	arqPort   *widget.Entry
	bcastPort *widget.Entry

	connectBtn    *widget.Button
	disconnectBtn *widget.Button
	arqConnect    *widget.Button
	arqDisconnect *widget.Button
	arqAbort      *widget.Button
	sendARQ       *widget.Button
	sendBcast     *widget.Button
	arqMsg        *widget.Entry
	bcastMsg      *widget.Entry
}

func (cw *chatWindow) build(app fyne.App, telemetry telemetryState) {
	cw.win = app.NewWindow("Mercury Client - Chat")

	cw.myCall = widget.NewEntry()
	cw.myCall.SetText(defaultCall(telemetry.UserCallsign, "NOCALL"))
	cw.target = widget.NewEntry()
	cw.target.SetText(defaultCall(telemetry.DestCallsign, "DEST"))
	cw.ip = widget.NewEntry()
	cw.ip.SetText("127.0.0.1")
	cw.arqPort = widget.NewEntry()
	cw.arqPort.SetText("8300")
	cw.bcastPort = widget.NewEntry()
	cw.bcastPort.SetText("8100")

	cw.arqMsg = widget.NewEntry()
	cw.arqMsg.SetPlaceHolder("ARQ message...")
	cw.bcastMsg = widget.NewEntry()
	cw.bcastMsg.SetPlaceHolder("Broadcast message...")

	cw.log = widget.NewMultiLineEntry()
	cw.log.SetPlaceHolder("Activity log...")
	cw.log.Wrapping = fyne.TextWrapBreak
	cw.log.Disable()

	cw.arq = widget.NewMultiLineEntry()
	cw.arq.SetPlaceHolder("ARQ chat")
	cw.arq.Wrapping = fyne.TextWrapBreak
	cw.arq.Disable()

	cw.bcast = widget.NewMultiLineEntry()
	cw.bcast.SetPlaceHolder("Broadcast chat")
	cw.bcast.Wrapping = fyne.TextWrapBreak
	cw.bcast.Disable()

	cw.connectBtn = widget.NewButton("Connect TCP", cw.onConnect)
	cw.disconnectBtn = widget.NewButton("Disconnect TCP", cw.onDisconnect)
	cw.disconnectBtn.Disable()
	cw.arqConnect = widget.NewButton("Connect ARQ", cw.onARQConnect)
	cw.arqConnect.Disable()
	cw.arqDisconnect = widget.NewButton("Disconnect ARQ", cw.onARQDisconnect)
	cw.arqDisconnect.Disable()
	cw.arqAbort = widget.NewButton("Abort", func() {
		if cw.mc != nil {
			cw.mc.AbortARQ()
		}
	})
	cw.arqAbort.Disable()
	cw.sendARQ = widget.NewButton("Send ARQ", cw.onSendARQ)
	cw.sendARQ.Disable()
	cw.sendBcast = widget.NewButton("Send Broadcast", cw.onSendBroadcast)
	cw.sendBcast.Disable()

	cfgForm := widget.NewForm(
		&widget.FormItem{Text: "My Callsign", Widget: cw.myCall},
		&widget.FormItem{Text: "Target Callsign", Widget: cw.target},
		&widget.FormItem{Text: "IP", Widget: cw.ip},
		&widget.FormItem{Text: "ARQ Port", Widget: cw.arqPort},
		&widget.FormItem{Text: "Broadcast Port", Widget: cw.bcastPort},
	)

	tcpRow := container.NewHBox(cw.connectBtn, cw.disconnectBtn)
	arqRow := container.NewHBox(cw.arqConnect, cw.arqDisconnect, cw.arqAbort)

	controls := container.NewVBox(
		cfgForm,
		widget.NewLabel("TCP:"), tcpRow,
		widget.NewLabel("ARQ:"), arqRow,
		widget.NewSeparator(),
		cw.arqMsg, cw.sendARQ,
		widget.NewSeparator(),
		cw.bcastMsg, cw.sendBcast,
	)

	left := container.NewVBox(controls, layout.NewSpacer())

	arqBox := container.NewBorder(
		widget.NewLabelWithStyle("ARQ Chat", fyne.TextAlignCenter, fyne.TextStyle{Bold: true}),
		nil, nil, nil, container.NewScroll(cw.arq),
	)
	bcastBox := container.NewBorder(
		widget.NewLabelWithStyle("Broadcast Chat", fyne.TextAlignCenter, fyne.TextStyle{Bold: true}),
		nil, nil, nil, container.NewScroll(cw.bcast),
	)
	logBox := container.NewBorder(
		widget.NewLabelWithStyle("Activity Log", fyne.TextAlignCenter, fyne.TextStyle{Bold: true}),
		nil, nil, nil, container.NewScroll(cw.log),
	)

	right := container.NewBorder(nil, nil, nil, nil,
		container.NewVSplit(
			container.NewVSplit(bcastBox, arqBox),
			logBox,
		),
	)

	cw.win.SetContent(container.NewHSplit(left, right))
	cw.win.Resize(fyne.NewSize(800, 600))
	cw.win.SetOnClosed(func() {
		if cw.mc != nil {
			cw.mc.Disconnect()
		}
	})
	cw.win.Show()
}

func (cw *chatWindow) logMsg(format string, args ...any) {
	fyne.Do(func() {
		ts := time.Now().Format("15:04:05")
		line := fmt.Sprintf("[%s] "+format, append([]any{ts}, args...)...)
		cur := cw.log.Text
		if cur == "" {
			cw.log.SetText(line)
		} else {
			cw.log.SetText(fmt.Sprintf("%s\n%s", line, cur))
		}
		cw.log.Refresh()
	})
}

func (cw *chatWindow) appendChat(entry *widget.Entry, line string) {
	fyne.Do(func() {
		cur := entry.Text
		if cur == "" {
			entry.SetText(line)
		} else {
			entry.SetText(fmt.Sprintf("%s\n%s", line, cur))
		}
		entry.Refresh()
	})
}

func (cw *chatWindow) setTCP(on bool) {
	fyne.Do(func() {
		if on {
			cw.connectBtn.Disable()
			cw.disconnectBtn.Enable()
			cw.arqConnect.Enable()
			cw.sendBcast.Enable()
		} else {
			cw.connectBtn.Enable()
			cw.disconnectBtn.Disable()
			cw.arqConnect.Disable()
			cw.arqDisconnect.Disable()
			cw.arqAbort.Disable()
			cw.sendARQ.Disable()
			cw.sendBcast.Disable()
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
		} else {
			cw.arqConnect.Enable()
			cw.arqDisconnect.Disable()
			cw.arqAbort.Disable()
			cw.sendARQ.Disable()
		}
	})
}

func (cw *chatWindow) onConnect() {
	arqPort, _ := strconv.Atoi(cw.arqPort.Text)
	bcastPort, _ := strconv.Atoi(cw.bcastPort.Text)
	cfg := client.Config{
		MyCallsign:     cw.myCall.Text,
		TargetCallsign: cw.target.Text,
		IP:             cw.ip.Text,
		ARQPort:        arqPort,
		BroadcastPort:  bcastPort,
	}
	mc := client.New(cfg)
	if err := mc.Connect(); err != nil {
		dialog.ShowError(err, cw.win)
		cw.logMsg("connect: %v", err)
		return
	}
	cw.mc = mc
	cw.setTCP(true)

	go cw.forwardLog()
	go cw.forwardARQChat()
	go cw.forwardBroadcastChat()
	go cw.forwardStatus()
}

func (cw *chatWindow) onDisconnect() {
	if cw.mc != nil {
		cw.mc.Disconnect()
		cw.mc = nil
	}
	cw.setTCP(false)
	cw.setARQ(false)
	cw.logMsg("Disconnected.")
}

func (cw *chatWindow) onARQConnect() {
	if cw.mc == nil || !cw.mc.IsConnected() {
		return
	}
	cw.logMsg("Connecting ARQ: %s -> %s", cw.myCall.Text, cw.target.Text)
	go func() {
		if err := cw.mc.ConnectARQ(); err != nil {
			cw.logMsg("ARQ connect: %v", err)
			cw.setARQ(false)
			return
		}
		cw.logMsg("ARQ connected.")
		cw.setARQ(true)
	}()
}

func (cw *chatWindow) onARQDisconnect() {
	if cw.mc != nil {
		cw.mc.DisconnectARQ()
	}
	cw.setARQ(false)
	cw.logMsg("ARQ disconnected.")
}

func (cw *chatWindow) onSendARQ() {
	if cw.mc == nil || !cw.mc.IsConnected() {
		return
	}
	msg := strings.TrimSpace(cw.arqMsg.Text)
	if msg == "" {
		return
	}
	if err := cw.mc.SendARQMessage(msg); err != nil {
		dialog.ShowError(err, cw.win)
		return
	}
	cw.arqMsg.SetText("")
}

func (cw *chatWindow) onSendBroadcast() {
	if cw.mc == nil || !cw.mc.IsConnected() {
		return
	}
	msg := strings.TrimSpace(cw.bcastMsg.Text)
	if msg == "" {
		return
	}
	if err := cw.mc.SendBroadcast(msg); err != nil {
		dialog.ShowError(err, cw.win)
		return
	}
	cw.bcastMsg.SetText("")
}

func (cw *chatWindow) forwardLog() {
	for m := range cw.mc.LogCh {
		cw.logMsg("%s", m)
	}
}

func (cw *chatWindow) forwardARQChat() {
	for m := range cw.mc.ARQChatCh {
		cw.appendChat(cw.arq, fmt.Sprintf("%s: %s", m.Call, m.Text))
	}
}

func (cw *chatWindow) forwardBroadcastChat() {
	for m := range cw.mc.BroadcastChatCh {
		cw.appendChat(cw.bcast, fmt.Sprintf("%s: %s", m.Call, m.Text))
	}
}

func (cw *chatWindow) forwardStatus() {
	for s := range cw.mc.StatusCh {
		cw.logMsg("TNC Status: %s", s)
		switch s {
		case "CONNECTED":
			cw.setARQ(true)
		case "DISCONNECTED":
			cw.setARQ(false)
		}
	}
}

func defaultCall(call, fallback string) string {
	if call != "" {
		return call
	}
	return fallback
}
