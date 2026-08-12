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

func openMercuryClientWindow(app fyne.App, telemetry telemetryState) {
	cw := &chatWindow{}
	cw.build(app, telemetry)
}

type chatWindow struct {
	win fyne.Window
	mc  *client.Client
	log *widget.Entry

	arqBox      *fyne.Container
	arqScroll   *container.Scroll
	bcastBox    *fyne.Container
	bcastScroll *container.Scroll

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
	cw.win = app.NewWindow("Mercury Client")

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
	cw.arqMsg.SetPlaceHolder("Type message to be sent...")
	cw.bcastMsg = widget.NewEntry()
	cw.bcastMsg.SetPlaceHolder("Type broadcast message...")

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
		if cw.mc != nil {
			cw.mc.AbortARQ()
		}
	})
	cw.arqAbort.Disable()
	cw.sendARQ = widget.NewButton("Send message", cw.onSendARQ)
	cw.sendARQ.Disable()
	cw.sendBcast = widget.NewButton("Broadcast message", cw.onSendBroadcast)
	cw.sendBcast.Disable()

	cfgForm := widget.NewForm(
		&widget.FormItem{Text: "My Callsign", Widget: cw.myCall},
		&widget.FormItem{Text: "Target Callsign", Widget: cw.target},
		&widget.FormItem{Text: "IP", Widget: cw.ip},
		&widget.FormItem{Text: "ARQ Port", Widget: cw.arqPort},
		&widget.FormItem{Text: "Broadcast Port", Widget: cw.bcastPort},
	)

	modemRow := container.NewHBox(cw.connectBtn, cw.disconnectBtn)
	sessionRow := container.NewHBox(cw.arqConnect, cw.arqDisconnect, cw.arqAbort)

	controls := container.NewVBox(
		cfgForm,
		modemRow,
		widget.NewLabel("Session"), sessionRow,
		widget.NewSeparator(),
		cw.arqMsg, cw.sendARQ,
		widget.NewSeparator(),
		cw.bcastMsg, cw.sendBcast,
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
		cw.appendRichChat(cw.arqBox, m.Call, m.Text)
	}
}

func (cw *chatWindow) forwardBroadcastChat() {
	for m := range cw.mc.BroadcastChatCh {
		call, text := splitCallText(m.Text)
		cw.appendRichChat(cw.bcastBox, call, text)
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
