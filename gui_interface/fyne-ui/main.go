package main

import (
	"context"
	"crypto/tls"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"image"
	"image/color"
	"math"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"
	"fyne.io/fyne/v2/canvas"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/layout"
	"fyne.io/fyne/v2/widget"
	"github.com/gorilla/websocket"
)

type appState struct {
	// mu guards the fields shared between the WebSocket reader goroutine
	// (writer) and the Fyne render/UI goroutines (readers): the connection
	// state and the spectrum/waterfall buffers drawn by the canvas rasters.
	mu               sync.RWMutex
	wsConn           *websocket.Conn
	wsContext        context.Context
	wsCancel         context.CancelFunc
	wsConnected      bool
	wsScheme         string
	wsHost           string
	wsPort           string
	captureItems     []optionItem
	playbackItems    []optionItem
	radioItems       []optionItem
	captureSelected  string
	playbackSelected string
	radioSelected    string
	radioDevicePath  string
	radioSerialSpeed string
	telemetry        telemetryState
	spectrumValues   []float32
	spectrumRate     int
	spectrumHistory  []float32
	waterfallRows    [][]float32
}

type optionItem struct {
	ID   string
	Name string
}

type telemetryState struct {
	Bitrate            int
	SNR                float64
	Sync               bool
	Direction          string
	UserCallsign       string
	DestCallsign       string
	ClientTCPConnected bool
	BytesTransmitted   int64
	BytesReceived      int64
	TXGainDB           float64
	TXPeakDBFS         float64
	Waterfall          bool
}

type uiBindings struct {
	statusLabel      *widget.Label
	wsStatusLabel    *widget.Label
	logBox           *widget.Entry
	captureSelect    *widget.Select
	playbackSelect   *widget.Select
	channelSelect    *widget.Select
	radioSelect      *widget.Select
	devicePathEntry  *widget.Entry
	serialSpeedEntry *widget.Select
	txGainLabel      *widget.Label
	txPeakLabel      *widget.Label
	bitrateLabel     *widget.Label
	snrLabel         *widget.Label
	directionLabel   *widget.Label
	callsLabel       *widget.Label
	tcpLabel         *widget.Label
	bytesLabel       *widget.Label
	// canvas texts for compact telemetry display
	bitrateText       *canvas.Text
	snrText           *canvas.Text
	directionText     *canvas.Text
	userCallsText     *canvas.Text
	destCallsText     *canvas.Text
	waterfallSNRText  *canvas.Text
	waterfallSyncText *canvas.Text
	tcpText           *canvas.Text
	txBytesText       *canvas.Text
	rxBytesText       *canvas.Text
	spectrumCanvas    *canvas.Raster
	waterfallCanvas   *canvas.Raster
}

func parseStatusMessage(payload []byte) (telemetryState, error) {
	var raw map[string]any
	if err := json.Unmarshal(payload, &raw); err != nil {
		return telemetryState{}, err
	}
	if raw["type"] != "status" {
		return telemetryState{}, fmt.Errorf("unexpected message type %v", raw["type"])
	}

	status := telemetryState{}
	status.Bitrate = int(toFloat64(raw["bitrate"]))
	status.SNR = toFloat64(raw["snr"])
	status.Sync = toBool(raw["sync"])
	status.Direction = strings.ToLower(fmt.Sprint(raw["direction"]))
	status.UserCallsign = fmt.Sprint(raw["user_callsign"])
	status.DestCallsign = fmt.Sprint(raw["dest_callsign"])
	status.ClientTCPConnected = toBool(raw["client_tcp_connected"])
	status.BytesTransmitted = int64(toFloat64(raw["bytes_transmitted"]))
	status.BytesReceived = int64(toFloat64(raw["bytes_received"]))
	status.TXGainDB = toFloat64(raw["tx_gain_db"])
	status.TXPeakDBFS = toFloat64(raw["tx_peak_dbfs"])
	status.Waterfall = toBool(raw["waterfall"])
	return status, nil
}

func parseSpectrumFrame(frame []byte) ([]float32, int, error) {
	const spectrumMagic = uint32(0x4D435259)
	if len(frame) < 8 {
		return nil, 0, fmt.Errorf("spectrum frame too short")
	}
	if binary.LittleEndian.Uint32(frame[0:4]) != spectrumMagic {
		return nil, 0, fmt.Errorf("invalid spectrum magic")
	}
	fftSize := int(binary.LittleEndian.Uint16(frame[4:6]))
	sampleRate := int(binary.LittleEndian.Uint16(frame[6:8]))
	if fftSize <= 0 || len(frame) < 8+fftSize*4 {
		return nil, 0, fmt.Errorf("spectrum frame payload too short")
	}
	values := make([]float32, fftSize)
	for i := 0; i < fftSize; i++ {
		bits := binary.LittleEndian.Uint32(frame[8+i*4:])
		values[i] = math.Float32frombits(bits)
	}
	return values, sampleRate, nil
}

func toFloat64(v any) float64 {
	switch value := v.(type) {
	case nil:
		return 0
	case float64:
		return value
	case float32:
		return float64(value)
	case json.Number:
		f, _ := value.Float64()
		return f
	case string:
		f, _ := strconv.ParseFloat(value, 64)
		return f
	default:
		return 0
	}
}

func toBool(v any) bool {
	switch value := v.(type) {
	case bool:
		return value
	case string:
		return strings.EqualFold(value, "true") || value == "1"
	case float64:
		return value != 0
	case int:
		return value != 0
	default:
		return false
	}
}

func runOnUI(fn func()) {
	fn()
}

func main() {
	// Announce the version on the terminal, just like the standalone daemon.
	mercuryPrintVersion()

	// Handle informational CLI actions (-h/-l/-z/-K) before touching the GUI,
	// so `mercury-ui -h` prints to the terminal and exits like the daemon.
	if mercuryInfoCheck(os.Args) {
		return
	}

	myApp := app.New()
	myWindow := myApp.NewWindow("Mercury Modem")
	myWindow.Resize(fyne.NewSize(1280, 780))

	state := &appState{wsScheme: "ws", wsHost: "127.0.0.1", wsPort: "10000"}
	bindings := &uiBindings{}

	statusLabel := widget.NewLabel("")
	bindings.statusLabel = statusLabel
	wsStatusLabel := widget.NewLabel("")
	bindings.wsStatusLabel = wsStatusLabel

	hostEntry := widget.NewEntry()
	hostEntry.SetText(state.wsHost)
	hostEntryBackground := canvas.NewRectangle(color.Transparent)
	hostEntryBackground.SetMinSize(fyne.NewSize(260, 34))
	hostEntryBox := container.NewMax(hostEntryBackground, hostEntry)

	portEntry := widget.NewEntry()
	portEntry.SetText(state.wsPort)
	portEntryBackground := canvas.NewRectangle(color.Transparent)
	portEntryBackground.SetMinSize(fyne.NewSize(110, 34))
	portEntryBox := container.NewMax(portEntryBackground, portEntry)

	schemeSelect := widget.NewSelect([]string{"ws", "wss"}, func(selected string) {
		state.wsScheme = selected
	})
	schemeSelect.SetSelected(state.wsScheme)
	schemeSelect.Resize(fyne.NewSize(90, 34))

	logBox := widget.NewMultiLineEntry()
	logBox.SetMinRowsVisible(8)
	logBox.SetText("--- Application Logs ---\n")
	bindings.logBox = logBox

	captureSelect := widget.NewSelect([]string{}, func(string) {})
	bindings.captureSelect = captureSelect
	playbackSelect := widget.NewSelect([]string{}, func(string) {})
	bindings.playbackSelect = playbackSelect
	channelSelect := widget.NewSelect([]string{"left", "right", "stereo"}, func(string) {})
	channelSelect.SetSelected("left")
	bindings.channelSelect = channelSelect
	radioSelect := widget.NewSelect([]string{}, func(string) {})
	bindings.radioSelect = radioSelect
	devicePathEntry := widget.NewEntry()
	devicePathEntry.SetPlaceHolder("/dev/ttyUSB0 or 127.0.0.1:4532")
	bindings.devicePathEntry = devicePathEntry
	serialSpeedEntry := widget.NewSelect([]string{"Auto", "4800", "9600", "19200", "38400", "115200"}, func(string) {})
	serialSpeedEntry.SetSelected("Auto")
	bindings.serialSpeedEntry = serialSpeedEntry

	txGainSlider := widget.NewSlider(-20.0, 20.0)
	txGainSlider.Step = 0.5
	txGainSlider.SetValue(0)
	txGainLabel := widget.NewLabel("TX gain: 0.0 dB")
	bindings.txGainLabel = txGainLabel
	txPeakLabel := widget.NewLabel("TX peak: --")
	bindings.txPeakLabel = txPeakLabel

	bitrateLabel := widget.NewLabel("Bitrate: --")
	bindings.bitrateLabel = bitrateLabel
	snrLabel := widget.NewLabel("SNR: --")
	bindings.snrLabel = snrLabel
	directionLabel := widget.NewLabel("Direction: --")
	bindings.directionLabel = directionLabel
	callsLabel := widget.NewLabel("Calls: --")
	bindings.callsLabel = callsLabel
	tcpLabel := widget.NewLabel("TCP: --")
	bindings.tcpLabel = tcpLabel
	bytesLabel := widget.NewLabel("Bytes: --")
	bindings.bytesLabel = bytesLabel

	// create compact telemetry canvas texts (smaller font sizes)
	bitrateText := canvas.NewText("0", color.NRGBA{R: 0xEE, G: 0xEE, B: 0xEE, A: 0xFF})
	bitrateText.TextSize = 14
	bitrateText.TextStyle = fyne.TextStyle{Bold: true}
	bindings.bitrateText = bitrateText

	snrText := canvas.NewText("-- dB", color.NRGBA{R: 0xEE, G: 0xEE, B: 0xEE, A: 0xFF})
	snrText.TextSize = 14
	bindings.snrText = snrText

	directionText := canvas.NewText("--", color.NRGBA{R: 0xDD, G: 0xDD, B: 0xDD, A: 0xFF})
	directionText.TextSize = 13
	bindings.directionText = directionText

	userCallsText := canvas.NewText("", color.NRGBA{R: 0xDD, G: 0xDD, B: 0xDD, A: 0xFF})
	userCallsText.TextSize = 13
	bindings.userCallsText = userCallsText

	destCallsText := canvas.NewText("", color.NRGBA{R: 0xDD, G: 0xDD, B: 0xDD, A: 0xFF})
	destCallsText.TextSize = 13
	bindings.destCallsText = destCallsText

	waterfallSNRText := canvas.NewText("SNR: -- dB", color.NRGBA{R: 0xEE, G: 0xEE, B: 0xEE, A: 0xFF})
	waterfallSNRText.TextSize = 14
	waterfallSNRText.TextStyle = fyne.TextStyle{Bold: true}
	bindings.waterfallSNRText = waterfallSNRText

	waterfallSyncText := canvas.NewText("", color.NRGBA{R: 0x66, G: 0xFF, B: 0x66, A: 0xFF})
	waterfallSyncText.TextSize = 14
	waterfallSyncText.TextStyle = fyne.TextStyle{Bold: true}
	bindings.waterfallSyncText = waterfallSyncText

	tcpText := canvas.NewText("--", color.NRGBA{R: 0xFF, G: 0x88, B: 0x88, A: 0xFF})
	tcpText.TextSize = 13
	bindings.tcpText = tcpText

	txBytesText := canvas.NewText("0", color.NRGBA{R: 0xDD, G: 0xDD, B: 0xDD, A: 0xFF})
	txBytesText.TextSize = 13
	bindings.txBytesText = txBytesText

	rxBytesText := canvas.NewText("0", color.NRGBA{R: 0xDD, G: 0xDD, B: 0xDD, A: 0xFF})
	rxBytesText.TextSize = 13
	bindings.rxBytesText = rxBytesText

	spectrumCanvas := canvas.NewRaster(func(w, h int) image.Image {
		img := image.NewNRGBA(image.Rect(0, 0, w, h))
		drawSpectrumImage(img, w, h, state)
		return img
	})
	bindings.spectrumCanvas = spectrumCanvas
	waterfallCanvas := canvas.NewRaster(func(w, h int) image.Image {
		img := image.NewNRGBA(image.Rect(0, 0, w, h))
		drawWaterfallImage(img, w, h, state)
		return img
	})
	bindings.waterfallCanvas = waterfallCanvas

	waterfallHeight := 180
	if myWindow.Canvas().Size().Width >= 1920 && myWindow.Canvas().Size().Height >= 1080 {
		waterfallHeight = int(myWindow.Canvas().Size().Height / 4)
	}
	waterfallBackground := canvas.NewRectangle(color.Transparent)
	waterfallBackground.SetMinSize(fyne.NewSize(0, float32(waterfallHeight)))
	waterfallCanvasBox := container.NewMax(waterfallBackground, waterfallCanvas)

	spectrumBackground := canvas.NewRectangle(color.Transparent)
	spectrumBackground.SetMinSize(fyne.NewSize(0, 90))
	spectrumCanvasBox := container.NewMax(spectrumBackground, spectrumCanvas)

	// set fixed spectrum and waterfall sizes so the display remains consistent
	spectrumCanvas.SetMinSize(fyne.NewSize(0, 90))
	waterfallCanvas.SetMinSize(fyne.NewSize(0, float32(waterfallHeight)))

	// open UI log file for appending; if it fails, uiLog will be nil and logs go only to the UI
	var uiLog *os.File
	uiLogPath := filepath.Join(getLogDir(), "ui.log")
	uiLog, _ = os.OpenFile(uiLogPath, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644)

	appendLog := func(msg string) {
		// write only to file by default; UI log widget removed from layout
		if bindings.logBox != nil {
			runOnUI(func() {
				bindings.logBox.SetText(bindings.logBox.Text + msg)
			})
		}
		if uiLog != nil {
			_, _ = uiLog.WriteString(msg)
		}
	}

	setWSStatus := func(text string) {}

	refreshSelect := func(selectWidget *widget.Select, items []optionItem, selectedID string, prependDefault bool) {
		labels := make([]string, 0, len(items)+1)
		if prependDefault {
			labels = append(labels, "default")
		}
		for _, item := range items {
			labels = append(labels, item.Name)
		}
		selectWidget.Options = labels
		selectWidget.Refresh()
		if selectedID != "" {
			for _, item := range items {
				if item.ID == selectedID {
					selectWidget.SetSelected(item.Name)
					return
				}
			}
			if prependDefault && selectedID == "default" {
				selectWidget.SetSelected("default")
				return
			}
		}
		if len(labels) > 0 {
			selectWidget.SetSelected(labels[0])
		}
	}

	selectedID := func(selectWidget *widget.Select, items []optionItem) string {
		if selectWidget.Selected == "" {
			return ""
		}
		for _, item := range items {
			if item.Name == selectWidget.Selected {
				return item.ID
			}
		}
		return selectWidget.Selected
	}

	refreshTelemetry := func() {
		runOnUI(func() {
			// update compact telemetry texts
			if bindings.bitrateText != nil {
				bindings.bitrateText.Text = fmt.Sprintf("%d", state.telemetry.Bitrate)
				bindings.bitrateText.Refresh()
			}
			if bindings.snrText != nil {
				bindings.snrText.Text = fmt.Sprintf("%.1f dB", state.telemetry.SNR)
				bindings.snrText.Refresh()
			}
			if bindings.directionText != nil {
				bindings.directionText.Text = strings.ToUpper(state.telemetry.Direction)
				bindings.directionText.Refresh()
			}
			if bindings.userCallsText != nil {
				bindings.userCallsText.Text = state.telemetry.UserCallsign
				bindings.userCallsText.Refresh()
			}
			if bindings.destCallsText != nil {
				bindings.destCallsText.Text = state.telemetry.DestCallsign
				bindings.destCallsText.Refresh()
			}
			if bindings.waterfallSNRText != nil {
				bindings.waterfallSNRText.Text = fmt.Sprintf("SNR: %.1f dB", state.telemetry.SNR)
				bindings.waterfallSNRText.Color = waterfallSNRColor(state.telemetry.SNR)
				bindings.waterfallSNRText.Refresh()
			}
			if bindings.waterfallSyncText != nil {
				if state.telemetry.Sync {
					bindings.waterfallSyncText.Text = "SYNC"
					bindings.waterfallSyncText.Color = color.NRGBA{R: 0x66, G: 0xFF, B: 0x66, A: 0xFF}
				} else {
					bindings.waterfallSyncText.Text = ""
				}
				bindings.waterfallSyncText.Refresh()
			}
			if bindings.tcpText != nil {
				if state.telemetry.ClientTCPConnected {
					bindings.tcpText.Text = "On"
					bindings.tcpText.Color = color.NRGBA{R: 0x66, G: 0xFF, B: 0x66, A: 0xFF}
				} else {
					bindings.tcpText.Text = "Off"
					bindings.tcpText.Color = color.NRGBA{R: 0xFF, G: 0x88, B: 0x88, A: 0xFF}
				}
				bindings.tcpText.Refresh()
			}
			if bindings.txBytesText != nil {
				bindings.txBytesText.Text = fmt.Sprintf("%d", state.telemetry.BytesTransmitted)
				bindings.txBytesText.Refresh()
			}
			if bindings.rxBytesText != nil {
				bindings.rxBytesText.Text = fmt.Sprintf("%d", state.telemetry.BytesReceived)
				bindings.rxBytesText.Refresh()
			}
			bindings.txGainLabel.SetText(fmt.Sprintf("TX gain: %.1f dB", state.telemetry.TXGainDB))
			bindings.txPeakLabel.SetText(fmt.Sprintf("TX peak: %.1f dBFS", state.telemetry.TXPeakDBFS))
		})
	}

	refreshSpectrum := func() {
		runOnUI(func() {
			bindings.spectrumCanvas.Refresh()
			bindings.waterfallCanvas.Refresh()
		})
	}

	applyStatus := func(status telemetryState) {
		state.telemetry = status
		refreshTelemetry()
		if len(state.spectrumValues) > 0 {
			refreshSpectrum()
		}
	}

	var connectionButtonState = "connect"
	var connectButton *widget.Button
	updateConnectionButton := func() {
		if connectButton == nil {
			return
		}
		switch connectionButtonState {
		case "connecting":
			connectButton.SetText("Connecting")
		case "disconnect":
			connectButton.SetText("Disconnect")
		default:
			connectButton.SetText("Connect")
		}
	}

	disconnectWS := func(reason string) {
		// Grab the connection handles under the lock, clear the shared state,
		// then Close()/cancel() outside the lock (no network calls held).
		state.mu.Lock()
		conn := state.wsConn
		cancel := state.wsCancel
		state.wsConn = nil
		state.wsCancel = nil
		state.wsConnected = false
		state.mu.Unlock()
		if conn != nil {
			_ = conn.Close()
		}
		if cancel != nil {
			cancel()
		}
		runOnUI(func() {
			connectionButtonState = "connect"
			updateConnectionButton()
		})
		setWSStatus("WebSocket: disconnected")
		if reason != "" {
			appendLog(reason + "\n")
		}
	}

	connectWS := func() {
		state.mu.Lock()
		if state.wsConnected && state.wsConn != nil {
			state.mu.Unlock()
			appendLog("WebSocket already connected.\n")
			return
		}
		oldCancel := state.wsCancel
		oldConn := state.wsConn
		state.wsConn = nil
		state.wsContext, state.wsCancel = context.WithCancel(context.Background())
		state.mu.Unlock()
		if oldCancel != nil {
			oldCancel()
		}
		if oldConn != nil {
			_ = oldConn.Close()
		}
		state.wsHost = hostEntry.Text
		state.wsPort = portEntry.Text
		hostEntry.SetText(state.wsHost)
		portEntry.SetText(state.wsPort)
		setWSStatus("WebSocket: connecting")
		appendLog(fmt.Sprintf("Connecting to WebSocket at %s://%s:%s/websocket...\n", state.wsScheme, state.wsHost, state.wsPort))

		go func() {
			var schemes []string
			if state.wsScheme == "wss" {
				schemes = []string{"wss", "ws"}
			} else {
				schemes = []string{"ws", "wss"}
			}
			var conn *websocket.Conn
			var err error
			for _, scheme := range schemes {
				wsURL := buildWebSocketURL(scheme, state.wsHost, state.wsPort)
				dialer := &websocket.Dialer{
					Proxy:            http.ProxyFromEnvironment,
					HandshakeTimeout: 5 * time.Second,
					TLSClientConfig:  &tls.Config{InsecureSkipVerify: true},
				}
				conn, _, err = dialer.Dial(wsURL, nil)
				if err == nil {
					state.wsScheme = scheme
					break
				}
			}
			if err != nil {
				runOnUI(func() {
					connectionButtonState = "connect"
					updateConnectionButton()
				})
				setWSStatus("WebSocket: disconnected")
				appendLog(fmt.Sprintf("WebSocket connection failed: %v\n", err))
				return
			}

			state.mu.Lock()
			state.wsConn = conn
			state.wsConnected = true
			ctx := state.wsContext
			state.mu.Unlock()
			runOnUI(func() {
				connectionButtonState = "disconnect"
				updateConnectionButton()
			})
			setWSStatus("WebSocket: connected")
			appendLog("Connected to Mercury WebSocket successfully!\n")

			go func() {
				<-ctx.Done()
				_ = conn.Close()
			}()

			for {
				select {
				case <-ctx.Done():
					return
				default:
					messageType, payload, readErr := conn.ReadMessage()
					if readErr != nil {
						disconnectWS(fmt.Sprintf("WebSocket read error: %v", readErr))
						return
					}
					switch messageType {
					case websocket.TextMessage:
						var raw map[string]any
						if err := json.Unmarshal(payload, &raw); err != nil {
							appendLog(fmt.Sprintf("[Raw WS Msg]: %s\n", string(payload)))
							continue
						}
						switch raw["type"] {
						case "status":
							status, parseErr := parseStatusMessage(payload)
							if parseErr != nil {
								appendLog(fmt.Sprintf("Failed to parse status: %v\n", parseErr))
								continue
							}
							applyStatus(status)
						case "capture_dev_list":
							items := parseMenuItems(payload)
							state.captureItems = items
							state.captureSelected = selectedValue(raw, "selected")
							refreshSelect(bindings.captureSelect, items, state.captureSelected, true)
						case "playback_dev_list":
							items := parseMenuItems(payload)
							state.playbackItems = items
							state.playbackSelected = selectedValue(raw, "selected")
							refreshSelect(bindings.playbackSelect, items, state.playbackSelected, true)
						case "input_channel":
							items := parseChannelItems(payload)
							refreshSelect(bindings.channelSelect, items, selectedValue(raw, "selected"), false)
						case "radio_list":
							items := parseMenuItems(payload)
							sort.Slice(items, func(i, j int) bool {
								if items[i].Name == "None" {
									return true
								}
								if items[j].Name == "None" {
									return false
								}
								return strings.ToLower(items[i].Name) < strings.ToLower(items[j].Name)
							})
							state.radioItems = items
							state.radioSelected = selectedValue(raw, "selected")
							state.radioDevicePath = fmt.Sprint(raw["device_path"])
							state.radioSerialSpeed = fmt.Sprint(raw["serial_speed"])
							if state.radioDevicePath != "" {
								bindings.devicePathEntry.SetText(state.radioDevicePath)
							}
							if state.radioSerialSpeed != "" {
								bindings.serialSpeedEntry.SetSelected(state.radioSerialSpeed)
							}
							refreshSelect(bindings.radioSelect, items, state.radioSelected, false)
						default:
							appendLog(fmt.Sprintf("[Raw WS Msg]: %s\n", string(payload)))
						}
					case websocket.BinaryMessage:
						spectrum, sampleRate, parseErr := parseSpectrumFrame(payload)
						if parseErr != nil {
							continue
						}
						// append a copy of this spectrum to the waterfall buffer
						row := make([]float32, len(spectrum))
						copy(row, spectrum)
						const maxWaterfallRows = 800
						state.mu.Lock()
						state.spectrumValues = spectrum
						state.spectrumRate = sampleRate
						state.waterfallRows = append(state.waterfallRows, row)
						if len(state.waterfallRows) > maxWaterfallRows {
							// drop oldest rows
							state.waterfallRows = state.waterfallRows[len(state.waterfallRows)-maxWaterfallRows:]
						}
						state.mu.Unlock()
						refreshSpectrum()
					}
				}
			}
		}()
	}

	sendWSCommand := func(command string, value string, value2 string, value3 string) error {
		state.mu.RLock()
		conn := state.wsConn
		connected := state.wsConnected
		state.mu.RUnlock()
		if conn == nil || !connected {
			return fmt.Errorf("not connected")
		}
		payload := map[string]any{"command": command, "value": value}
		if value2 != "" {
			payload["value2"] = value2
		}
		if value3 != "" {
			payload["value3"] = value3
		}
		return conn.WriteJSON(payload)
	}

	connectButton = widget.NewButton("Connect", func() {
		switch connectionButtonState {
		case "connecting":
			return
		case "disconnect":
			disconnectWS("Disconnect requested by user.")
			connectionButtonState = "connect"
			updateConnectionButton()
		default:
			state.mu.RLock()
			alreadyConnected := state.wsConnected && state.wsConn != nil
			state.mu.RUnlock()
			if alreadyConnected {
				connectionButtonState = "disconnect"
				updateConnectionButton()
				return
			}
			connectionButtonState = "connecting"
			updateConnectionButton()
			connectWS()
		}
	})

	audioApplyButton := widget.NewButton("Apply", func() {
		captureID := selectedID(bindings.captureSelect, state.captureItems)
		playbackID := selectedID(bindings.playbackSelect, state.playbackItems)
		channel := bindings.channelSelect.Selected
		if captureID == "" {
			captureID = "default"
		}
		if playbackID == "" {
			playbackID = "default"
		}
		if channel == "" {
			channel = "left"
		}
		if err := sendWSCommand("set_audio_config", captureID, playbackID, channel); err != nil {
			appendLog(fmt.Sprintf("Failed to send audio config: %v\n", err))
		} else {
			appendLog(fmt.Sprintf("Sent audio config: capture=%s playback=%s channel=%s\n", captureID, playbackID, channel))
		}
	})

	radioApplyButton := widget.NewButton("Apply", func() {
		modelID := selectedID(bindings.radioSelect, state.radioItems)
		if modelID == "" {
			appendLog("Select a radio model before applying.\n")
			return
		}
		devPath := bindings.devicePathEntry.Text
		serialSpeed := bindings.serialSpeedEntry.Selected
		if serialSpeed == "" || serialSpeed == "Auto" {
			serialSpeed = "0"
		}
		if err := sendWSCommand("set_radio_config", modelID, devPath, serialSpeed); err != nil {
			appendLog(fmt.Sprintf("Failed to send radio config: %v\n", err))
		} else {
			appendLog(fmt.Sprintf("Sent radio config: model=%s path=%s baud=%s\n", modelID, devPath, serialSpeed))
		}
	})

	txGainSlider.OnChanged = func(value float64) {
		bindings.txGainLabel.SetText(fmt.Sprintf("TX gain: %.1f dB", value))
		if err := sendWSCommand("set_tx_gain", fmt.Sprintf("%.2f", value), "", ""); err != nil {
			appendLog(fmt.Sprintf("Failed to send TX gain: %v\n", err))
		}
	}

	topBar := container.NewHBox()

	connectionCard := widget.NewCard("", "", container.NewHBox(
		container.NewVBox(widget.NewLabel("Host"), hostEntryBox),
		container.NewVBox(widget.NewLabel("Port"), portEntryBox),
		container.NewVBox(widget.NewLabel("Scheme"), schemeSelect),
		layout.NewSpacer(),
		connectButton,
	))

	audioCard := widget.NewCard("", "", container.NewVBox(
		container.NewGridWithColumns(2,
			widget.NewLabel("Capture Device"), captureSelect,
			widget.NewLabel("Playback Device"), playbackSelect,
			widget.NewLabel("Capture Input Channel"), channelSelect,
		),
		audioApplyButton,
	))

	radioCard := widget.NewCard("", "", container.NewVBox(
		container.NewGridWithColumns(2,
			widget.NewLabel("Radio model"), radioSelect,
			widget.NewLabel("Device path"), devicePathEntry,
			widget.NewLabel("Baud Rate"), serialSpeedEntry,
		),
		radioApplyButton,
	))

	txCard := widget.NewCard("", "", container.NewVBox(
		bindings.txGainLabel,
		txGainSlider,
		bindings.txPeakLabel,
	))

	// compact telemetry layout matching screenshot: left labels small, right values small-bold
	telemetryGrid := container.NewGridWithColumns(2,
		canvas.NewText("Bitrate", color.NRGBA{R: 0xAA, G: 0xAA, B: 0xAA, A: 0xFF}), bindings.bitrateText,
		canvas.NewText("Direction", color.NRGBA{R: 0xAA, G: 0xAA, B: 0xAA, A: 0xFF}), bindings.directionText,
		canvas.NewText("User Callsign", color.NRGBA{R: 0xAA, G: 0xAA, B: 0xAA, A: 0xFF}), bindings.userCallsText,
		canvas.NewText("Dest Callsign", color.NRGBA{R: 0xAA, G: 0xAA, B: 0xAA, A: 0xFF}), bindings.destCallsText,
		canvas.NewText("Client TCP Connected", color.NRGBA{R: 0xAA, G: 0xAA, B: 0xAA, A: 0xFF}), bindings.tcpText,
		canvas.NewText("Bytes transmitted", color.NRGBA{R: 0xAA, G: 0xAA, B: 0xAA, A: 0xFF}), bindings.txBytesText,
		canvas.NewText("Bytes received", color.NRGBA{R: 0xAA, G: 0xAA, B: 0xAA, A: 0xFF}), bindings.rxBytesText,
	)
	telemetryCard := widget.NewCard("Telemetry", "", telemetryGrid)

	waterfallTop := container.NewMax(
		canvas.NewRectangle(color.NRGBA{R: 0x00, G: 0x00, B: 0x00, A: 0xA0}),
		container.NewHBox(
			bindings.waterfallSNRText,
			layout.NewSpacer(),
			bindings.waterfallSyncText,
		),
	)
	waterfallBottom := container.NewGridWithColumns(7,
		func() fyne.CanvasObject {
			t := canvas.NewText("0", color.NRGBA{R: 0xCC, G: 0xCC, B: 0xCC, A: 0xFF})
			t.TextSize = 8
			return t
		}(),
		func() fyne.CanvasObject {
			t := canvas.NewText("500", color.NRGBA{R: 0xCC, G: 0xCC, B: 0xCC, A: 0xFF})
			t.TextSize = 8
			return t
		}(),
		func() fyne.CanvasObject {
			t := canvas.NewText("1000", color.NRGBA{R: 0xCC, G: 0xCC, B: 0xCC, A: 0xFF})
			t.TextSize = 8
			return t
		}(),
		func() fyne.CanvasObject {
			t := canvas.NewText("1500", color.NRGBA{R: 0xCC, G: 0xCC, B: 0xCC, A: 0xFF})
			t.TextSize = 8
			return t
		}(),
		func() fyne.CanvasObject {
			t := canvas.NewText("2000", color.NRGBA{R: 0xCC, G: 0xCC, B: 0xCC, A: 0xFF})
			t.TextSize = 8
			return t
		}(),
		func() fyne.CanvasObject {
			t := canvas.NewText("2500", color.NRGBA{R: 0xCC, G: 0xCC, B: 0xCC, A: 0xFF})
			t.TextSize = 8
			return t
		}(),
		func() fyne.CanvasObject {
			t := canvas.NewText("3000", color.NRGBA{R: 0xCC, G: 0xCC, B: 0xCC, A: 0xFF})
			t.TextSize = 8
			return t
		}(),
	)
	waterfallContent := container.NewBorder(
		waterfallTop,
		waterfallBottom,
		nil,
		nil,
		waterfallCanvasBox,
	)
	spectrumCard := widget.NewCard("", "", container.NewBorder(
		spectrumCanvasBox,
		nil,
		nil,
		nil,
		waterfallContent,
	))
	const fixedWaterfallHeight = 310
	spectrumCard.Resize(fyne.NewSize(0, fixedWaterfallHeight))

	topPanel := container.NewVBox(
		connectionCard,
		container.NewGridWithColumns(2,
			container.NewVBox(audioCard, radioCard),
			container.NewVBox(txCard, telemetryCard),
		),
	)
	content := container.NewBorder(nil, spectrumCard, nil, nil, container.NewVScroll(topPanel))

	mainLayout := container.NewBorder(topBar, nil, nil, nil, content)
	myWindow.SetContent(mainLayout)

	myWindow.SetOnClosed(func() {
		state.mu.Lock()
		conn := state.wsConn
		cancel := state.wsCancel
		state.wsConn = nil
		state.wsCancel = nil
		state.wsConnected = false
		state.mu.Unlock()
		if conn != nil {
			_ = conn.Close()
		}
		if cancel != nil {
			cancel()
		}
		mercuryStop()
		if uiLog != nil {
			_ = uiLog.Close()
		}
	})

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sigCh
		appendLog("Signal received, shutting down...\n")
		myWindow.Close()
	}()

	go func() {
		time.Sleep(200 * time.Millisecond)

		// Per-user writable config used when no -C is passed (seeded from the
		// bundled default the first time).  The C parser picks -C over this.
		defaultConfig := filepath.Join(getLogDir(), "mercury.ini")
		if _, err := os.Stat(defaultConfig); os.IsNotExist(err) {
			bundled := filepath.Join(getBaseDir(), "mercury.ini")
			if data, rerr := os.ReadFile(bundled); rerr == nil {
				os.WriteFile(defaultConfig, data, 0644)
			}
		}

		logPath := filepath.Join(getLogDir(), "mercury_engine.log")
		appendLog("Starting Mercury engine...\n")
		// Forward the process args so the engine's own CLI parser applies them.
		if err := mercuryStart(defaultConfig, logPath, os.Args); err != nil {
			appendLog(fmt.Sprintf("Failed to start Mercury engine: %v\n", err))
			return
		}
		appendLog("Mercury engine started — connecting...\n")
		connectWS()
	}()

	myWindow.ShowAndRun()
}

func parseMenuItems(payload []byte) []optionItem {
	var raw map[string]any
	if err := json.Unmarshal(payload, &raw); err != nil {
		return nil
	}
	list, ok := raw["list"].([]any)
	if !ok {
		return nil
	}
	items := make([]optionItem, 0, len(list))
	for _, item := range list {
		entry, ok := item.(map[string]any)
		if !ok {
			continue
		}
		name := fmt.Sprint(entry["name"])
		id := fmt.Sprint(entry["id"])
		items = append(items, optionItem{ID: id, Name: name})
	}
	return items
}

func parseChannelItems(payload []byte) []optionItem {
	var raw map[string]any
	if err := json.Unmarshal(payload, &raw); err != nil {
		return nil
	}
	list, ok := raw["list"].([]any)
	if !ok {
		return nil
	}
	items := make([]optionItem, 0, len(list))
	for _, item := range list {
		items = append(items, optionItem{ID: fmt.Sprint(item), Name: fmt.Sprint(item)})
	}
	return items
}

func selectedValue(raw map[string]any, key string) string {
	if raw[key] == nil {
		return ""
	}
	return fmt.Sprint(raw[key])
}

func drawSpectrumImage(img *image.NRGBA, w, h int, state *appState) {
	// background
	bg := color.NRGBA{R: 0x06, G: 0x06, B: 0x10, A: 0xFF}
	for x := 0; x < w; x++ {
		for y := 0; y < h; y++ {
			img.SetNRGBA(x, y, bg)
		}
	}
	// Snapshot the shared slice header under the lock; spectrumValues is
	// replaced wholesale by the reader goroutine (never mutated in place),
	// so the snapshot is safe to iterate after unlocking.
	state.mu.RLock()
	vals := state.spectrumValues
	state.mu.RUnlock()
	if len(vals) == 0 {
		return
	}
	ctx := &spectrumContext{img: img, w: w, h: h}
	ctx.drawGrid()
	// draw the spectrum line with a bold cyan color
	ctx.drawLine(vals)
	// draw a faint filled area under the line
	ctx.fillUnderLine(vals)
}

func drawWaterfallImage(img *image.NRGBA, w, h int, state *appState) {
	// clear background
	bg := color.NRGBA{R: 0x00, G: 0x00, B: 0x08, A: 0xFF}
	for x := 0; x < w; x++ {
		for y := 0; y < h; y++ {
			img.SetNRGBA(x, y, bg)
		}
	}
	// Snapshot the outer slice header under the lock.  The reader goroutine
	// appends/reslices waterfallRows, but each row is created once and never
	// mutated, so iterating the snapshot after unlocking is safe.
	state.mu.RLock()
	rows := state.waterfallRows
	state.mu.RUnlock()
	if len(rows) == 0 {
		return
	}
	// determine how many rows to draw (newest at top)
	rowsToDraw := h
	if len(rows) < rowsToDraw {
		rowsToDraw = len(rows)
	}
	// draw newest row at the top (rowIdx 0 -> newest)
	for rowIdx := 0; rowIdx < rowsToDraw; rowIdx++ {
		row := rows[len(rows)-1-rowIdx]
		destY := rowIdx
		for x := 0; x < w; x++ {
			if len(row) == 0 {
				continue
			}
			srcIdx := (x * len(row)) / w
			v := float64(row[srcIdx])
			if math.IsNaN(v) {
				continue
			}
			c := waterfallColorForDB(v)
			img.SetNRGBA(x, destY, c)
		}
	}
}

func waterfallColorForDB(v float64) color.NRGBA {
	// normalize roughly from -100..0 dB into 0..1
	t := (v + 70.0) / 70.0
	if t < 0 {
		t = 0
	}
	if t > 1 {
		t = 1
	}
	// gradient: dark blue -> blue -> cyan -> green -> yellow
	switch {
	case t < 0.25:
		// deep blue
		f := t / 0.25
		return color.NRGBA{R: uint8(0x00 * (1 - f)), G: uint8(0x00 * (1 - f)), B: uint8(0x20 + int(200*f)), A: 0xFF}
	case t < 0.5:
		f := (t - 0.25) / 0.25
		return color.NRGBA{R: 0x00, G: uint8(0x20 * f), B: 0xFF, A: 0xFF}
	case t < 0.75:
		f := (t - 0.5) / 0.25
		return color.NRGBA{R: 0x00, G: uint8(0x80 * f), B: uint8(0xFF - int(0x80*f)), A: 0xFF}
	default:
		f := (t - 0.75) / 0.25
		return color.NRGBA{R: uint8(0x00 + int(0xFF*f)), G: uint8(0xCC), B: 0x00, A: 0xFF}
	}
}

type spectrumContext struct {
	img *image.NRGBA
	w   int
	h   int
}

func (c *spectrumContext) drawGrid() {
	for y := 0; y < c.h; y += 20 {
		for x := 0; x < c.w; x++ {
			c.img.SetNRGBA(x, y, color.NRGBA{R: 0x20, G: 0x20, B: 0x30, A: 0x80})
		}
	}
}

func (c *spectrumContext) drawLine(values []float32) {
	if len(values) == 0 {
		return
	}
	for x := 0; x < c.w; x++ {
		idx := int(float64(x) / float64(c.w) * float64(len(values)-1))
		value := values[idx]
		normalized := clamp((float64(value)+50.0)/80.0, 0.0, 1.0)
		scaled := int((1.0 - normalized) * float64(c.h-1))
		scaled = int(clamp(float64(scaled), 0.0, float64(c.h-1)))
		// draw primary pixel
		c.img.SetNRGBA(x, scaled, color.NRGBA{R: 0x00, G: 0xCC, B: 0xFF, A: 0xFF})
		// draw glow above
		if scaled > 0 {
			c.img.SetNRGBA(x, scaled-1, color.NRGBA{R: 0x00, G: 0x88, B: 0xCC, A: 0xC0})
		}
		if scaled > 1 {
			c.img.SetNRGBA(x, scaled-2, color.NRGBA{R: 0x00, G: 0x44, B: 0x88, A: 0x80})
		}
	}
}

func (c *spectrumContext) fillUnderLine(values []float32) {
	if len(values) == 0 {
		return
	}
	for x := 0; x < c.w; x++ {
		idx := int(float64(x) / float64(c.w) * float64(len(values)-1))
		value := values[idx]
		normalized := clamp((float64(value)+50.0)/80.0, 0.0, 1.0)
		scaled := int((1.0 - normalized) * float64(c.h-1))
		scaled = int(clamp(float64(scaled), 0.0, float64(c.h-1)))
		// fill a faint gradient below the line
		for y := scaled; y < c.h; y++ {
			alpha := uint8(10 + int(40*(float64(y-scaled)/float64(c.h))))
			existing := c.img.NRGBAAt(x, y)
			// blend toward black-green tint
			newC := color.NRGBA{R: uint8((int(existing.R)*(255-int(alpha)) + 0*int(alpha)) / 255), G: uint8((int(existing.G)*(255-int(alpha)) + 20*int(alpha)) / 255), B: uint8((int(existing.B)*(255-int(alpha)) + 40*int(alpha)) / 255), A: 0xFF}
			c.img.SetNRGBA(x, y, newC)
		}
	}
}

func colorForDB(v float64) color.NRGBA {
	c := clamp((v+50)/80.0, 0, 1)
	if c < 0.2 {
		return color.NRGBA{R: 0x00, G: 0x00, B: 0x20, A: 0xFF}
	}
	if c < 0.6 {
		return color.NRGBA{R: 0x00, G: 0x20, B: 0x40, A: 0xFF}
	}
	return color.NRGBA{R: 0x00, G: 0xCC, B: 0xFF, A: 0xFF}
}

func waterfallSNRColor(v float64) color.NRGBA {
	if v >= 10 {
		return color.NRGBA{R: 0x66, G: 0xFF, B: 0x66, A: 0xFF}
	}
	if v >= 0 {
		return color.NRGBA{R: 0xCC, G: 0xFF, B: 0x66, A: 0xFF}
	}
	if v >= -10 {
		return color.NRGBA{R: 0xFF, G: 0xDD, B: 0x66, A: 0xFF}
	}
	if v >= -20 {
		return color.NRGBA{R: 0xFF, G: 0x88, B: 0x66, A: 0xFF}
	}
	return color.NRGBA{R: 0xFF, G: 0x66, B: 0x66, A: 0xFF}
}

func clamp(value, min, max float64) float64 {
	if value < min {
		return min
	}
	if value > max {
		return max
	}
	return value
}

func buildWebSocketURL(scheme, host, port string) string {
	u := url.URL{Scheme: scheme, Host: netJoinHostPort(host, port), Path: "/websocket"}
	return u.String()
}

func netJoinHostPort(host, port string) string {
	if host == "" {
		host = "127.0.0.1"
	}
	if port == "" {
		port = "10000"
	}
	return net.JoinHostPort(host, port)
}

func getBaseDir() string {
	if runtime.GOOS == "windows" {
		if exePath, err := os.Executable(); err == nil {
			return filepath.Dir(exePath)
		}
	}
	return "."
}

func getLogDir() string {
	if runtime.GOOS == "windows" {
		dir := filepath.Join(os.Getenv("LOCALAPPDATA"), "MercuryHF")
		os.MkdirAll(dir, 0755)
		return dir
	}
	return "."
}
