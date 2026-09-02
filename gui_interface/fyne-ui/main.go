package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"image"
	"image/color"
	"math"
	"net"
	"net/url"
	"os"
	"os/signal"
	"path/filepath"
	"reflect"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"
	"fyne.io/fyne/v2/canvas"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/driver/desktop"
	"fyne.io/fyne/v2/layout"
	"fyne.io/fyne/v2/widget"
)

type appState struct {
	// mu guards the fields shared between the WebSocket reader goroutine
	// (writer) and the Fyne render/UI goroutines (readers): the connection
	// state and the spectrum/waterfall buffers drawn by the canvas rasters.
	mu               sync.RWMutex
	link             Link
	wsContext        context.Context
	wsCancel         context.CancelFunc
	wsConnected      bool
	wsScheme         string
	useRemote        bool
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
	pttMethod        string
	pttLine          string
	pttInvert        string
	cm108GPIO        string
	telemetry        telemetryState
	spectrumValues   []float32
	spectrumRate     int
	spectrumHistory  []float32
	waterfallRows    [][]float32
	waterfallPalette string
}

type optionItem struct {
	ID   string
	Name string
}

var pttMethodLabels = map[string]string{
	"none":       "None",
	"hamlib":     "Hamlib",
	"serial":     "Serial (RTS/DTR)",
	"cm108":      "CM108 GPIO",
	"hermes_shm": "HERMES SHM",
}

var pttMethodOrder = []string{"none", "hamlib", "serial", "cm108", "hermes_shm"}

func pttMethodOptions() []string {
	options := make([]string, 0, len(pttMethodOrder))
	for _, method := range pttMethodOrder {
		options = append(options, pttMethodLabel(method))
	}
	return options
}

func pttMethodLabel(method string) string {
	if label, ok := pttMethodLabels[method]; ok {
		return label
	}
	return pttMethodLabels["none"]
}

func pttMethodID(label string) string {
	for method, candidate := range pttMethodLabels {
		if candidate == label {
			return method
		}
	}
	return "none"
}

// Mirrors UI_SNR_UNKNOWN_DB in gui_interface/ui_status.h.
const snrUnknownDB = -99.9

type telemetryState struct {
	Bitrate int
	SNR     float64
	// SNR the far side reports for OUR signal.  PeerSNRValid is false until
	// a reading has actually arrived (issue #230).
	PeerSNR            float64
	PeerSNRValid       bool
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
	AudioOk            bool
	AudioError         string
}

type uiBindings struct {
	statusLabel      *widget.Label
	wsStatusLabel    *widget.Label
	logBox           *widget.Entry
	captureSelect    *widget.Select
	playbackSelect   *widget.Select
	channelSelect    *widget.Select
	pttMethodSelect  *widget.Select
	radioSelect      *widget.Select
	devicePathEntry  *widget.Entry
	serialSpeedEntry *widget.Select
	pttLineSelect    *widget.Select
	pttInvertSelect  *widget.Select
	cm108GPIOSelect  *widget.Select
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
	peerSnrText       *canvas.Text
	snrRowLabel       *canvas.Text
	peerSnrRowLabel   *canvas.Text
	directionText     *canvas.Text
	directionDot      *canvas.Circle
	userCallsText     *canvas.Text
	destCallsText     *canvas.Text
	waterfallSNRText  *canvas.Text
	waterfallSyncText *canvas.Text
	tcpText           *canvas.Text
	txBytesText       *canvas.Text
	rxBytesText       *canvas.Text
	spectrumCanvas    *canvas.Raster
	waterfallCanvas   *canvas.Raster
	waterfallCard     *widget.Card
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
	// Default to the sentinel, not 0.0: a server that predates issue #230 sends
	// neither field, and 0.0 would render as a real "they hear us at 0 dB".
	status.PeerSNR = snrUnknownDB
	status.PeerSNRValid = false
	if v, ok := raw["peer_snr_valid"].(bool); ok && v {
		status.PeerSNRValid = true
		status.PeerSNR = toFloat64(raw["peer_snr"])
	}
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
	if v, present := raw["audio_ok"]; present {
		status.AudioOk = toBool(v)
	} else {
		// An older engine does not send the field; treat it as healthy rather
		// than nagging with a spurious "device error" popup.
		status.AudioOk = true
	}
	status.AudioError = toString(raw["audio_error"])
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

// toString renders a JSON value as a string, but unlike fmt.Sprint it maps a
// missing field (nil) to "" rather than "<nil>" — the caller wants an empty
// string when the field is absent, not a literal "<nil>" to show the operator.
func toString(v any) string {
	if v == nil {
		return ""
	}
	return fmt.Sprint(v)
}

// runOnUI runs fn on Fyne's main goroutine. Fyne v2.6+ requires all widget
// updates to happen there; fyne.Do queues fn when the event loop is running
// and runs it inline during setup/shutdown, so it is safe from any goroutine.
func runOnUI(fn func()) {
	fyne.Do(fn)
}

const (
	// Minimum heights for the two plots. Kept low deliberately: they are floors
	// that must fit an 800x480 panel, not the size the plots are drawn at --
	// the split below stretches them to fill a desktop window.
	spectrumMinHeight  = 60
	waterfallMinHeight = 90
	// Share of the window given to the controls above the plots. Roughly
	// matches the previous desktop proportions.
	waterfallSplitOffset = 0.60
)

const (
	windowWidthKey  = "window.width"
	windowHeightKey = "window.height"
	windowXKey      = "window.x"
	windowYKey      = "window.y"
	windowPosSetKey = "window.positionSaved"

	// Broadcast file receiving.  Persisted because a station that exists to
	// collect bulletins is configured once and then left alone: it must come
	// back up receiving into the same folder after a restart, with nobody
	// present to tick a box.
	broadcastRxDirPrefKey = "broadcastFile.rxDir"
	broadcastRxOnPrefKey  = "broadcastFile.receive"

	// defaultWindowWidth/Height match the previous hard-coded launch size and
	// are used until the operator has resized the window once.
	defaultWindowWidth  = 1280
	defaultWindowHeight = 780
)

// saveWindowGeometry records the current window size and position in the app
// preferences so the next launch can restore them.
func saveWindowGeometry(win fyne.Window, prefs fyne.Preferences) {
	if prefs == nil {
		return
	}
	size := win.Canvas().Size()
	if size.Width > 0 && size.Height > 0 {
		prefs.SetFloat(windowWidthKey, float64(size.Width))
		prefs.SetFloat(windowHeightKey, float64(size.Height))
	}
	if x, y, ok := windowPosition(win); ok {
		prefs.SetInt(windowXKey, x)
		prefs.SetInt(windowYKey, y)
		prefs.SetBool(windowPosSetKey, true)
	}
}

// restoreWindowGeometry applies a previously saved window size and position.
func restoreWindowGeometry(win fyne.Window, prefs fyne.Preferences) {
	if prefs == nil {
		return
	}
	width := prefs.FloatWithFallback(windowWidthKey, defaultWindowWidth)
	height := prefs.FloatWithFallback(windowHeightKey, defaultWindowHeight)
	if width <= 0 {
		width = defaultWindowWidth
	}
	if height <= 0 {
		height = defaultWindowHeight
	}
	win.Resize(fyne.NewSize(float32(width), float32(height)))

	if prefs.BoolWithFallback(windowPosSetKey, false) {
		if dw, ok := win.(desktop.Window); ok {
			dw.RequestPosition(prefs.Int(windowXKey), prefs.Int(windowYKey))
		}
	}
}

// windowPosition reads the native window position from the desktop driver's
// concrete window. Fyne's public Window interface exposes no position getter,
// so this reaches into the (pinned) driver struct via reflection.
func windowPosition(win fyne.Window) (int, int, bool) {
	v := reflect.ValueOf(win)
	if v.Kind() != reflect.Ptr {
		return 0, 0, false
	}
	v = v.Elem()
	if v.Kind() != reflect.Struct {
		return 0, 0, false
	}
	xField := v.FieldByName("xpos")
	yField := v.FieldByName("ypos")
	if !xField.IsValid() || !yField.IsValid() ||
		xField.Kind() != reflect.Int || yField.Kind() != reflect.Int {
		return 0, 0, false
	}
	return int(xField.Int()), int(yField.Int()), true
}

func main() {
	// Announce the version on the terminal, just like the standalone daemon.
	mercuryPrintVersion()

	// Handle informational CLI actions (-h/-l/-z/-K) before touching the GUI,
	// so `mercury-ui -h` prints to the terminal and exits like the daemon.
	if mercuryInfoCheck(os.Args) {
		return
	}

	// Use a stable application ID (matches the .desktop / Mercury.app appID) so
	// Fyne's preferences/storage have a unique identity instead of warning.
	myApp := app.NewWithID("org.rhizomatica.mercury")
	myWindow := myApp.NewWindow("Mercury Modem")
	restoreWindowGeometry(myWindow, myApp.Preferences())

	state := &appState{wsScheme: "ws", wsHost: "127.0.0.1", wsPort: "10000",
		waterfallPalette: myApp.Preferences().StringWithFallback("waterfallPalette", "blackblue")}
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

	// Set once mercuryStart() has returned. Until then the embedded engine's
	// getters have no context to read (g_ui_ctx is still NULL), so a link
	// opened before that point comes up with empty device and radio lists and
	// never retries -- the pickers stay stuck on "(Select one)".
	var engineReady atomic.Bool

	// Declared here because the Engine selector below drives them, while they
	// are built further down with the rest of the UI.
	var (
		connectionButtonState  = "connect"
		connectButton          *widget.Button
		updateConnectionButton func()
		connectLink            func()
		disconnectLink         func(reason string)
	)

	// Where the engine runs. "Local" drives the engine inside this binary
	// through CGo and opens no socket at all; "Remote" reaches a Mercury on
	// another machine over the websocket, using the fields beside it. The
	// fields stay visible either way so it is obvious what Remote would use,
	// but they are only editable when they apply.
	setRemoteFieldsEnabled := func(enabled bool) {
		if enabled {
			hostEntry.Enable()
			portEntry.Enable()
			schemeSelect.Enable()
			return
		}
		hostEntry.Disable()
		portEntry.Disable()
		schemeSelect.Disable()
	}

	engineSelect := widget.NewSelect([]string{"Local", "Remote"}, func(selected string) {
		wasRemote := state.useRemote
		state.useRemote = selected == "Remote"
		setRemoteFieldsEnabled(state.useRemote)

		// SetSelected() fires this during construction, before the link
		// helpers further down exist — hence the nil checks.
		if state.useRemote {
			// Leaving Local: drop the in-process link, then wait for the
			// operator to dial the remote one.
			if !wasRemote && disconnectLink != nil {
				disconnectLink("Switched to a remote engine.")
			}
			connectionButtonState = "connect"
			if updateConnectionButton != nil {
				updateConnectionButton()
			}
			return
		}

		// Entering Local: the engine is right here, so attach to it rather
		// than making the operator press a button to reach their own modem.
		if wasRemote && disconnectLink != nil {
			disconnectLink("Switched to the local engine.")
		}
		if updateConnectionButton != nil {
			updateConnectionButton()
		}
		// At startup this fires before the engine has been started; the
		// goroutine that starts it connects afterwards. Connecting here too
		// would win the race and pin empty pickers for the whole session.
		if connectLink != nil && engineReady.Load() {
			connectLink()
		}
	})
	engineSelect.Resize(fyne.NewSize(110, 34))

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
	pttMethodSelect := widget.NewSelect(pttMethodOptions(), func(string) {})
	pttMethodSelect.SetSelected(pttMethodLabel("none"))
	bindings.pttMethodSelect = pttMethodSelect
	radioSelect := widget.NewSelect([]string{}, func(string) {})
	bindings.radioSelect = radioSelect
	devicePathEntry := widget.NewEntry()
	devicePathEntry.SetPlaceHolder("/dev/ttyUSB0 or 127.0.0.1:4532")
	bindings.devicePathEntry = devicePathEntry
	serialSpeedEntry := widget.NewSelect([]string{"Auto", "4800", "9600", "19200", "38400", "115200"}, func(string) {})
	serialSpeedEntry.SetSelected("Auto")
	bindings.serialSpeedEntry = serialSpeedEntry
	pttLineSelect := widget.NewSelect([]string{"rts", "dtr", "both"}, func(string) {})
	pttLineSelect.SetSelected("rts")
	bindings.pttLineSelect = pttLineSelect
	pttInvertSelect := widget.NewSelect([]string{"none", "rts", "dtr", "both"}, func(string) {})
	pttInvertSelect.SetSelected("none")
	bindings.pttInvertSelect = pttInvertSelect
	cm108GPIOSelect := widget.NewSelect([]string{"1", "2", "3", "4"}, func(string) {})
	cm108GPIOSelect.SetSelected("3")
	bindings.cm108GPIOSelect = cm108GPIOSelect

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

	// What the far side reports hearing from us.  This is the number that
	// tells an operator whether their TX audio level is right: lower the
	// drive and watch it go UP if the rig was over-driven (issue #230).
	peerSnrText := canvas.NewText("-- dB", color.NRGBA{R: 0x88, G: 0x88, B: 0x88, A: 0xFF})
	peerSnrText.TextSize = 14
	bindings.peerSnrText = peerSnrText

	// SNR row in the Telemetry card, shown only when the waterfall (which
	// carries its own SNR overlay) is disabled.
	snrRowLabel := canvas.NewText("SNR", color.NRGBA{R: 0xAA, G: 0xAA, B: 0xAA, A: 0xFF})
	bindings.snrRowLabel = snrRowLabel

	peerSnrRowLabel := canvas.NewText("SNR (them)", color.NRGBA{R: 0xAA, G: 0xAA, B: 0xAA, A: 0xFF})
	bindings.peerSnrRowLabel = peerSnrRowLabel

	directionText := canvas.NewText("--", color.NRGBA{R: 0xDD, G: 0xDD, B: 0xDD, A: 0xFF})
	directionText.TextSize = 20
	bindings.directionText = directionText

	directionDot := canvas.NewCircle(color.NRGBA{R: 0xDD, G: 0xDD, B: 0xDD, A: 0xFF})
	bindings.directionDot = directionDot

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

	// Floor, not the display size: the split below hands the waterfall a share
	// of whatever the window has, so it is large on a desktop without this
	// number claiming space. It has to stay small enough that a 800x480 panel
	// (a Pi with the official display) still has room for the controls --
	// at 180 the spectrum card's minimum alone swallowed the whole window and
	// the pickers, radio fields and telemetry vanished entirely.
	waterfallHeight := waterfallMinHeight
	waterfallBackground := canvas.NewRectangle(color.Transparent)
	waterfallBackground.SetMinSize(fyne.NewSize(0, float32(waterfallHeight)))
	waterfallCanvasBox := container.NewMax(waterfallBackground, waterfallCanvas)

	spectrumBackground := canvas.NewRectangle(color.Transparent)
	spectrumBackground.SetMinSize(fyne.NewSize(0, spectrumMinHeight))
	spectrumCanvasBox := container.NewMax(spectrumBackground, spectrumCanvas)

	// Minimums only; the split decides how much each actually gets.
	spectrumCanvas.SetMinSize(fyne.NewSize(0, spectrumMinHeight))
	waterfallCanvas.SetMinSize(fyne.NewSize(0, float32(waterfallHeight)))

	// open UI log file for appending; if it fails, uiLog will be nil and logs go only to the UI
	var uiLog *os.File

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

	logDir := getLogDir()
	if err := os.MkdirAll(logDir, 0755); err != nil {
		appendLog(fmt.Sprintf("Failed to create logs directory %s: %v\n", logDir, err))
	}
	uiLogPath := filepath.Join(logDir, "ui.log")
	if f, err := os.OpenFile(uiLogPath, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644); err != nil {
		appendLog(fmt.Sprintf("Failed to open log file %s: %v\n", uiLogPath, err))
	} else {
		uiLog = f
	}

	setWSStatus := func(text string) {}

	refreshSelect := func(selectWidget *widget.Select, items []optionItem, selectedID string, prependDefault bool) {
		runOnUI(func() {
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
		})
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
			// Snapshot once, under the lock, then render from the copy: this
			// closure runs on the GL thread while the link goroutine is still
			// publishing new status.
			state.mu.Lock()
			telemetry := state.telemetry
			state.mu.Unlock()

			// update compact telemetry texts
			if bindings.bitrateText != nil {
				bindings.bitrateText.Text = fmt.Sprintf("%d", telemetry.Bitrate)
				bindings.bitrateText.Refresh()
			}
			if bindings.snrText != nil {
				bindings.snrText.Text = fmt.Sprintf("%.1f dB", telemetry.SNR)
				// "--" until they have reported: a literal 0.0 would read as
				// "they hear us at zero", which is the opposite of the truth.
				if telemetry.PeerSNRValid {
					bindings.peerSnrText.Text = fmt.Sprintf("%.1f dB", telemetry.PeerSNR)
					bindings.peerSnrText.Color = color.NRGBA{R: 0xEE, G: 0xEE, B: 0xEE, A: 0xFF}
				} else {
					bindings.peerSnrText.Text = "-- dB"
					bindings.peerSnrText.Color = color.NRGBA{R: 0x88, G: 0x88, B: 0x88, A: 0xFF}
				}
				bindings.peerSnrText.Refresh()
				bindings.snrText.Color = waterfallSNRColor(telemetry.SNR)
				bindings.snrText.Refresh()
			}
			if bindings.directionText != nil {
				bindings.directionText.Text = strings.ToUpper(telemetry.Direction)
				txRed := color.NRGBA{R: 0xFF, G: 0x44, B: 0x44, A: 0xFF}
				rxGreen := color.NRGBA{R: 0x44, G: 0xFF, B: 0x44, A: 0xFF}
				switch strings.ToUpper(telemetry.Direction) {
				case "TX":
					bindings.directionText.Color = txRed
					if bindings.directionDot != nil {
						bindings.directionDot.FillColor = txRed
						bindings.directionDot.Show()
					}
				case "RX":
					bindings.directionText.Color = rxGreen
					if bindings.directionDot != nil {
						bindings.directionDot.FillColor = rxGreen
						bindings.directionDot.Show()
					}
				default:
					bindings.directionText.Color = color.NRGBA{R: 0xDD, G: 0xDD, B: 0xDD, A: 0xFF}
					if bindings.directionDot != nil {
						bindings.directionDot.Hide()
					}
				}
				bindings.directionText.Refresh()
			}
			if bindings.userCallsText != nil {
				bindings.userCallsText.Text = telemetry.UserCallsign
				bindings.userCallsText.Refresh()
			}
			if bindings.destCallsText != nil {
				bindings.destCallsText.Text = telemetry.DestCallsign
				bindings.destCallsText.Refresh()
			}
			if bindings.waterfallSNRText != nil {
				peer := "--"
				if telemetry.PeerSNRValid {
					peer = fmt.Sprintf("%.1f dB", telemetry.PeerSNR)
				}
				bindings.waterfallSNRText.Text = fmt.Sprintf("SNR: %.1f dB  them: %s", telemetry.SNR, peer)
				bindings.waterfallSNRText.Color = waterfallSNRColor(telemetry.SNR)
				bindings.waterfallSNRText.Refresh()
			}
			if bindings.waterfallSyncText != nil {
				if telemetry.Sync {
					bindings.waterfallSyncText.Text = "SYNC"
					bindings.waterfallSyncText.Color = color.NRGBA{R: 0x66, G: 0xFF, B: 0x66, A: 0xFF}
				} else {
					bindings.waterfallSyncText.Text = ""
				}
				bindings.waterfallSyncText.Refresh()
			}
			if bindings.tcpText != nil {
				if telemetry.ClientTCPConnected {
					bindings.tcpText.Text = "On"
					bindings.tcpText.Color = color.NRGBA{R: 0x66, G: 0xFF, B: 0x66, A: 0xFF}
				} else {
					bindings.tcpText.Text = "Off"
					bindings.tcpText.Color = color.NRGBA{R: 0xFF, G: 0x88, B: 0x88, A: 0xFF}
				}
				bindings.tcpText.Refresh()
			}
			if bindings.txBytesText != nil {
				bindings.txBytesText.Text = fmt.Sprintf("%d", telemetry.BytesTransmitted)
				bindings.txBytesText.Refresh()
			}
			if bindings.rxBytesText != nil {
				bindings.rxBytesText.Text = fmt.Sprintf("%d", telemetry.BytesReceived)
				bindings.rxBytesText.Refresh()
			}
			bindings.txGainLabel.SetText(fmt.Sprintf("TX gain: %.1f dB", telemetry.TXGainDB))
			bindings.txPeakLabel.SetText(fmt.Sprintf("TX peak: %.1f dBFS", telemetry.TXPeakDBFS))
			if bindings.waterfallCard != nil {
				if telemetry.Waterfall {
					bindings.waterfallCard.Show()
				} else {
					bindings.waterfallCard.Hide()
				}
			}
			// The waterfall carries its own SNR overlay, so the Telemetry SNR row
			// is only shown when the waterfall is disabled (and thus hidden).
			if bindings.snrRowLabel != nil && bindings.snrText != nil {
				if telemetry.Waterfall {
					bindings.snrRowLabel.Hide()
					bindings.snrText.Hide()
					bindings.peerSnrRowLabel.Hide()
					bindings.peerSnrText.Hide()
				} else {
					bindings.snrRowLabel.Show()
					bindings.snrText.Show()
					bindings.peerSnrRowLabel.Show()
					bindings.peerSnrText.Show()
				}
			}
		})
	}

	refreshSpectrum := func() {
		runOnUI(func() {
			bindings.spectrumCanvas.Refresh()
			bindings.waterfallCanvas.Refresh()
		})
	}

	// Audio health popup state. applyStatus runs on the single link goroutine,
	// so plain captured booleans are safe here; they track the previous status
	// so the dialog fires once per failure, not on every 500 ms poll.
	var prevAudioOk bool
	var seenStatus bool

	applyStatus := func(status telemetryState) {
		// Under the lock: the link goroutine writes this while the GL thread
		// reads it in refreshTelemetry(). telemetryState holds strings, so an
		// unsynchronised write can hand the reader a torn string header and
		// segfault the app -- the race detector flags every field of it.
		state.mu.Lock()
		state.telemetry = status
		haveSpectrum := len(state.spectrumValues) > 0
		state.mu.Unlock()
		refreshTelemetry()
		if haveSpectrum {
			refreshSpectrum()
		}

		// A device that fails to open, or negotiates a rate the modem cannot
		// use, otherwise leaves the UI showing a healthy station that hears
		// nothing -- so surface it as a dialog on the transition to failure.
		if !status.AudioOk && (!seenStatus || prevAudioOk) {
			msg := status.AudioError
			if msg == "" {
				msg = "The audio device could not be started."
			}
			runOnUI(func() {
				dialog.ShowInformation("Audio Device Error", msg, myWindow)
			})
		}
		prevAudioOk = status.AudioOk
		seenStatus = true
	}

	updateConnectionButton = func() {
		if connectButton == nil {
			return
		}
		// In Local mode there is nothing to dial: the engine is already
		// running inside this process, started before the window opened, and
		// "Disconnect" would only blind the UI while the modem carried on
		// transmitting. The button belongs to the remote session.
		if !state.useRemote {
			connectButton.Hide()
			return
		}
		connectButton.Show()
		switch connectionButtonState {
		case "connecting":
			connectButton.SetText("Connecting")
		case "disconnect":
			connectButton.SetText("Disconnect")
		default:
			connectButton.SetText("Connect")
		}
	}

	disconnectLink = func(reason string) {
		// Grab the connection handles under the lock, clear the shared state,
		// then Close()/cancel() outside the lock (no network calls held).
		state.mu.Lock()
		link := state.link
		cancel := state.wsCancel
		state.link = nil
		state.wsCancel = nil
		state.wsConnected = false
		state.mu.Unlock()
		if link != nil {
			link.Close()
		}
		if cancel != nil {
			cancel()
		}
		runOnUI(func() {
			connectionButtonState = "connect"
			updateConnectionButton()
		})
		setWSStatus("Engine: disconnected")
		if reason != "" {
			appendLog(reason + "\n")
		}
	}

	connectLink = func() {
		state.mu.Lock()
		if state.wsConnected && state.link != nil {
			state.mu.Unlock()
			appendLog("Engine already connected.\n")
			return
		}
		oldCancel := state.wsCancel
		oldLink := state.link
		state.link = nil
		state.wsContext, state.wsCancel = context.WithCancel(context.Background())
		state.mu.Unlock()
		if oldCancel != nil {
			oldCancel()
		}
		if oldLink != nil {
			oldLink.Close()
		}
		state.wsHost = hostEntry.Text
		state.wsPort = portEntry.Text
		runOnUI(func() {
			hostEntry.SetText(state.wsHost)
			portEntry.SetText(state.wsPort)
		})
		setWSStatus("Engine: connecting")
		appendLog(fmt.Sprintf("Connecting to WebSocket at %s://%s:%s/websocket...\n", state.wsScheme, state.wsHost, state.wsPort))

		go func() {
			// Pick the transport: the engine linked into this binary when we
			// have one and no remote host was given, otherwise the websocket.
			// Everything below this point is transport-agnostic — see link.go.
			link, err := openLink(state.useRemote, state.wsHost, state.wsPort, state.wsScheme)
			if err != nil {
				runOnUI(func() {
					connectionButtonState = "connect"
					updateConnectionButton()
				})
				setWSStatus("Engine: disconnected")
				appendLog(fmt.Sprintf("Link failed: %v\n", err))
				return
			}

			state.mu.Lock()
			state.link = link
			state.wsConnected = true
			ctx := state.wsContext
			state.mu.Unlock()

			events, err := link.Start(ctx)
			if err != nil {
				link.Close()
				state.mu.Lock()
				state.link = nil
				state.wsConnected = false
				state.mu.Unlock()
				runOnUI(func() {
					connectionButtonState = "connect"
					updateConnectionButton()
				})
				setWSStatus("Engine: disconnected")
				appendLog(fmt.Sprintf("Link failed: %v\n", err))
				return
			}

			runOnUI(func() {
				connectionButtonState = "disconnect"
				updateConnectionButton()
			})
			setWSStatus("Engine: " + link.Name())
			appendLog("Connected to " + link.Name() + "\n")

			for ev := range events {
				switch e := ev.(type) {
				case StatusEvent:
					applyStatus(e.Status)

				case SpectrumEvent:
					// Keep a copy: the waterfall holds rows well past this frame.
					row := make([]float32, len(e.Bins))
					copy(row, e.Bins)
					const maxWaterfallRows = 800
					state.mu.Lock()
					state.spectrumValues = e.Bins
					state.spectrumRate = e.SampleRate
					state.waterfallRows = append(state.waterfallRows, row)
					if len(state.waterfallRows) > maxWaterfallRows {
						state.waterfallRows = state.waterfallRows[len(state.waterfallRows)-maxWaterfallRows:]
					}
					state.mu.Unlock()
					refreshSpectrum()

				case DeviceListEvent:
					switch e.Kind {
					case DeviceCapture:
						state.captureItems = e.Items
						state.captureSelected = e.Selected
						refreshSelect(bindings.captureSelect, e.Items, e.Selected, true)
					case DevicePlayback:
						state.playbackItems = e.Items
						state.playbackSelected = e.Selected
						refreshSelect(bindings.playbackSelect, e.Items, e.Selected, true)
					case DeviceInputChannel:
						refreshSelect(bindings.channelSelect, e.Items, e.Selected, false)
					}

				case RadioListEvent:
					state.radioItems = e.Items
					state.radioSelected = e.Selected
					state.radioDevicePath = e.DevicePath
					state.radioSerialSpeed = e.SerialSpeed
					state.pttMethod = e.PTTMethod
					state.pttLine = e.PTTLine
					state.pttInvert = e.PTTInvert
					state.cm108GPIO = e.CM108GPIO
					runOnUI(func() {
						bindings.pttMethodSelect.SetSelected(pttMethodLabel(e.PTTMethod))
						if e.DevicePath != "" {
							bindings.devicePathEntry.SetText(e.DevicePath)
						}
						if e.SerialSpeed != "" {
							bindings.serialSpeedEntry.SetSelected(e.SerialSpeed)
						}
						bindings.pttLineSelect.SetSelected(e.PTTLine)
						bindings.pttInvertSelect.SetSelected(e.PTTInvert)
						bindings.cm108GPIOSelect.SetSelected(e.CM108GPIO)
					})
					refreshSelect(bindings.radioSelect, e.Items, e.Selected, false)

				case LinkStateEvent:
					if !e.Up {
						disconnectLink(e.Detail)
						return
					}
					appendLog(e.Detail + "\n")

				case LogEvent:
					appendLog(e.Text)
				}
			}

			disconnectLink("Link closed.")
		}()
	}

	// One command path for both transports: in-process call or websocket
	// frame, decided by whichever Link is open.
	sendWSCommand := func(command string, value string, value2 string, value3 string, value4 string,
		value5 string, value6 string, value7 string) error {
		state.mu.RLock()
		link := state.link
		connected := state.wsConnected
		state.mu.RUnlock()
		if link == nil || !connected {
			return fmt.Errorf("not connected")
		}
		return link.Send(Command{Name: command, Value: value, Value2: value2, Value3: value3,
			Value4: value4, Value5: value5, Value6: value6, Value7: value7})
	}

	connectButton = widget.NewButton("Connect", func() {
		switch connectionButtonState {
		case "connecting":
			return
		case "disconnect":
			disconnectLink("Disconnect requested by user.")
			connectionButtonState = "connect"
			updateConnectionButton()
		default:
			state.mu.RLock()
			alreadyConnected := state.wsConnected && state.link != nil
			state.mu.RUnlock()
			if alreadyConnected {
				connectionButtonState = "disconnect"
				updateConnectionButton()
				return
			}
			connectionButtonState = "connecting"
			updateConnectionButton()
			connectLink()
		}
	})

	txGainSlider.OnChanged = func(value float64) {
		bindings.txGainLabel.SetText(fmt.Sprintf("TX gain: %.1f dB", value))
		if err := sendWSCommand("set_tx_gain", fmt.Sprintf("%.2f", value), "", "", "", "", "", ""); err != nil {
			appendLog(fmt.Sprintf("Failed to send TX gain: %v\n", err))
		}
	}

	mercuryClientButton := widget.NewButton("Launch Mercury Client", func() {
		state.mu.RLock()
		tel := state.telemetry
		link := state.link
		state.mu.RUnlock()
		arqPort, broadcastPort := 8300, 8100
		if engLink, ok := link.(*engineLink); ok {
			arqPort, broadcastPort = engLink.TCPPorts()
		}
		openMercuryClientWindow(myApp, tel, arqPort, broadcastPort)
	})

	topBar := container.NewHBox(
		layout.NewSpacer(),
		mercuryClientButton,
	)

	txCard := widget.NewCard("", "", container.NewVBox(
		bindings.txGainLabel,
		txGainSlider,
		bindings.txPeakLabel,
	))

	// compact telemetry layout matching screenshot: left labels small, right values small-bold
	txrxLabel := canvas.NewText("TX/RX", color.NRGBA{R: 0xAA, G: 0xAA, B: 0xAA, A: 0xFF})
	txrxLabel.TextSize = directionText.TextSize
	telemetryGrid := container.NewGridWithColumns(2,
		txrxLabel,
		container.NewHBox(
			container.NewVBox(layout.NewSpacer(), container.NewGridWrap(fyne.NewSize(directionText.TextSize, directionText.TextSize), bindings.directionDot), layout.NewSpacer()),
			bindings.directionText,
		),
		canvas.NewText("Bitrate", color.NRGBA{R: 0xAA, G: 0xAA, B: 0xAA, A: 0xFF}), bindings.bitrateText,
		bindings.snrRowLabel, bindings.snrText,
		bindings.peerSnrRowLabel, bindings.peerSnrText,
		canvas.NewText("My callsign", color.NRGBA{R: 0xAA, G: 0xAA, B: 0xAA, A: 0xFF}), bindings.userCallsText,
		canvas.NewText("Target callsign", color.NRGBA{R: 0xAA, G: 0xAA, B: 0xAA, A: 0xFF}), bindings.destCallsText,
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
	bindings.waterfallCard = spectrumCard

	topPanel := container.NewVBox(
		container.NewVBox(txCard, telemetryCard),
	)
	// A split, not a border: with the spectrum as a border's bottom edge it was
	// handed its full minimum height before anything else, so on a short screen
	// it took the lot and the controls above it collapsed to a sliver. A split
	// gives each side a proportion of the window and lets the operator drag the
	// divider, which is the only way a 480px-tall panel can show both.
	content := container.NewVSplit(container.NewVScroll(topPanel), spectrumCard)
	content.SetOffset(waterfallSplitOffset)

	mainLayout := container.NewBorder(topBar, nil, nil, nil, content)
	myWindow.SetContent(mainLayout)

	showSoundcardDialog := func() {
		applyBtn := widget.NewButton("Apply", func() {
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
			if err := sendWSCommand("set_audio_config", captureID, playbackID, channel, "", "", "", ""); err != nil {
				appendLog(fmt.Sprintf("Failed to send audio config: %v\n", err))
			} else {
				appendLog(fmt.Sprintf("Sent audio config: capture=%s playback=%s channel=%s\n",
					captureID, playbackID, channel))
			}
		})

		content := container.NewVBox(
			container.NewGridWithColumns(2,
				widget.NewLabel("Capture Device"), bindings.captureSelect,
				widget.NewLabel("Playback Device"), bindings.playbackSelect,
				widget.NewLabel("Capture Input Channel"), bindings.channelSelect,
			),
			container.NewHBox(layout.NewSpacer(), applyBtn, layout.NewSpacer()),
		)

		dialog.ShowCustom("Soundcards", "Close", content, myWindow)
	}

	showRadioDialog := func() {
		applyBtn := widget.NewButton("Apply", func() {
			method := pttMethodID(bindings.pttMethodSelect.Selected)
			modelID := selectedID(bindings.radioSelect, state.radioItems)
			if method == "hamlib" && (modelID == "" || modelID == "-1") {
				appendLog("Select a Hamlib radio model before applying.\n")
				return
			}
			devPath := bindings.devicePathEntry.Text
			if method == "serial" && devPath == "" {
				appendLog("Enter the serial device used for RTS PTT.\n")
				return
			}
			serialSpeed := bindings.serialSpeedEntry.Selected
			if serialSpeed == "" || serialSpeed == "Auto" {
				serialSpeed = "0"
			}
			pttLine := bindings.pttLineSelect.Selected
			pttInvert := bindings.pttInvertSelect.Selected
			cm108GPIO := bindings.cm108GPIOSelect.Selected
			if err := sendWSCommand("set_ptt_config", method, devPath, modelID, serialSpeed,
				pttLine, pttInvert, cm108GPIO); err != nil {
				appendLog(fmt.Sprintf("Failed to send PTT config: %v\n", err))
			} else {
				appendLog(fmt.Sprintf("Sent PTT config: method=%s model=%s path=%s baud=%s line=%s invert=%s gpio=%s\n",
					method, modelID, devPath, serialSpeed, pttLine, pttInvert, cm108GPIO))
			}
		})

		deviceRow := container.NewGridWithColumns(2,
			widget.NewLabel("Device Path"), bindings.devicePathEntry)
		modelRow := container.NewGridWithColumns(2,
			widget.NewLabel("Hamlib Model"), bindings.radioSelect)
		baudRow := container.NewGridWithColumns(2,
			widget.NewLabel("Hamlib Baud Rate"), bindings.serialSpeedEntry)
		lineRow := container.NewGridWithColumns(2,
			widget.NewLabel("PTT Line"), bindings.pttLineSelect)
		invertRow := container.NewGridWithColumns(2,
			widget.NewLabel("Invert Line"), bindings.pttInvertSelect)
		gpioRow := container.NewGridWithColumns(2,
			widget.NewLabel("CM108 GPIO"), bindings.cm108GPIOSelect)
		previousMethod := pttMethodID(bindings.pttMethodSelect.Selected)
		updateMethodFields := func(label string) {
			method := pttMethodID(label)
			if method == "cm108" && (previousMethod == "serial" || previousMethod == "hamlib") {
				bindings.devicePathEntry.SetText("")
			}
			previousMethod = method
			lineRow.Hide()
			invertRow.Hide()
			gpioRow.Hide()
			switch method {
			case "hamlib":
				deviceRow.Show()
				modelRow.Show()
				baudRow.Show()
			case "serial":
				deviceRow.Show()
				modelRow.Hide()
				baudRow.Hide()
				lineRow.Show()
				invertRow.Show()
			case "cm108":
				// Device is auto-detected; an explicit /dev/hidrawN is the
				// override, so the field stays available but is not required.
				deviceRow.Show()
				modelRow.Hide()
				baudRow.Hide()
				gpioRow.Show()
			default:
				deviceRow.Hide()
				modelRow.Hide()
				baudRow.Hide()
			}
		}
		bindings.pttMethodSelect.OnChanged = updateMethodFields

		content := container.NewVBox(
			container.NewGridWithColumns(2,
				widget.NewLabel("PTT Method"), bindings.pttMethodSelect),
			deviceRow,
			modelRow,
			baudRow,
			lineRow,
			invertRow,
			gpioRow,
			container.NewHBox(layout.NewSpacer(), applyBtn, layout.NewSpacer()),
		)
		updateMethodFields(bindings.pttMethodSelect.Selected)

		bg := canvas.NewRectangle(color.Transparent)
		bg.SetMinSize(fyne.NewSize(500, 0))
		padded := container.NewStack(bg, container.NewPadded(content))

		dialog.ShowCustom("PTT Configuration", "Close", padded, myWindow)
	}

	showWaterfallDialog := func() {
		paletteOpts := []string{"Turbo", "Hot", "Grayscale", "Blackblue"}
		paletteSelect := widget.NewSelect(paletteOpts, nil)
		upperToLower := map[string]string{
			"Turbo": "turbo", "Hot": "hot", "Grayscale": "grayscale", "Blackblue": "blackblue",
		}
		lowerToUpper := map[string]string{
			"turbo": "Turbo", "hot": "Hot", "grayscale": "Grayscale", "blackblue": "Blackblue",
		}
		paletteSelect.SetSelected(lowerToUpper[state.waterfallPalette])

		enabledCheck := widget.NewCheck("Waterfall enabled", nil)
		state.mu.RLock()
		enabledCheck.Checked = state.telemetry.Waterfall
		state.mu.RUnlock()
		enabledCheck.OnChanged = func(on bool) {
			val := "on"
			if !on {
				val = "off"
			}
			state.mu.RLock()
			engLink, isEngine := state.link.(*engineLink)
			state.mu.RUnlock()
			if isEngine {
				engLink.SetWaterfall(on)
				appendLog(fmt.Sprintf("Waterfall turned %s.\n", val))
				return
			}
			if err := sendWSCommand("set_waterfall", val, "", "", "", "", "", ""); err != nil {
				appendLog(fmt.Sprintf("Failed to toggle waterfall: %v\n", err))
			}
		}

		applyBtn := widget.NewButton("Apply", func() {
			sel := paletteSelect.Selected
			if sel == "" {
				return
			}
			internal := upperToLower[sel]
			state.mu.Lock()
			state.waterfallPalette = internal
			state.mu.Unlock()
			myApp.Preferences().SetString("waterfallPalette", internal)
			appendLog(fmt.Sprintf("Waterfall palette: %s\n", sel))
		})

		content := container.NewVBox(
			enabledCheck,
			widget.NewSeparator(),
			container.NewGridWithColumns(2,
				widget.NewLabel("Color Palette"), paletteSelect,
			),
			container.NewHBox(layout.NewSpacer(), applyBtn, layout.NewSpacer()),
		)

		bg := canvas.NewRectangle(color.Transparent)
		bg.SetMinSize(fyne.NewSize(400, 0))
		padded := container.NewStack(bg, container.NewPadded(content))
		dialog.ShowCustom("Waterfall", "Close", padded, myWindow)
	}

	showRemoteControlDialog := func() {
		content := container.NewVBox(
			engineSelect,
			container.NewGridWithColumns(2,
				widget.NewLabel("IP/Host"), hostEntryBox,
				widget.NewLabel("UI Port"), portEntryBox,
				widget.NewLabel("Scheme"), schemeSelect,
			),
			container.NewPadded(connectButton),
		)

		dialog.ShowCustom("Remote Control", "Close", content, myWindow)
	}

	remoteItem := fyne.NewMenuItem("Connect to remote host", showRemoteControlDialog)
	remoteMenu := fyne.NewMenu("Remote Control", remoteItem)
	soundcardsItem := fyne.NewMenuItem("Soundcards", showSoundcardDialog)
	radioConfigItem := fyne.NewMenuItem("Radio Config", showRadioDialog)
	waterfallItem := fyne.NewMenuItem("Waterfall", showWaterfallDialog)
	configMenu := fyne.NewMenu("Settings", soundcardsItem, radioConfigItem, waterfallItem)

	openExternalURL := func(raw string) {
		u, err := url.Parse(raw)
		if err != nil {
			appendLog(fmt.Sprintf("Invalid URL: %v\n", err))
			return
		}
		if err := myApp.OpenURL(u); err != nil {
			appendLog(fmt.Sprintf("Failed to open %s: %v\n", raw, err))
		}
	}

	openLogsDir := func() {
		dir := getLogDir()
		p := filepath.ToSlash(dir)
		if runtime.GOOS == "windows" && !strings.HasPrefix(p, "/") {
			p = "/" + p
		}
		if err := myApp.OpenURL(&url.URL{Scheme: "file", Path: p}); err != nil {
			appendLog(fmt.Sprintf("Failed to open logs directory %s: %v\n", dir, err))
		}
	}

	showVersionDialog := func() {
		version, gitHash := "unknown", "unknown000"
		state.mu.RLock()
		engLink, isEngine := state.link.(*engineLink)
		state.mu.RUnlock()
		if isEngine {
			version, gitHash = engLink.Version()
		}
		dialog.ShowInformation("Version",
			fmt.Sprintf("Mercury Version: %s\nGit Commit: %s", version, gitHash), myWindow)
	}

	versionItem := fyne.NewMenuItem("Version", showVersionDialog)
	supportItem := fyne.NewMenuItem("Support Us", func() {
		openExternalURL("https://www.paypal.com/donate/?hosted_button_id=EKY4LRAH64Z9S")
	})
	mailingItem := fyne.NewMenuItem("Join our mailing list", func() {
		openExternalURL("https://lists.riseup.net/www/info/hermes-general")
	})
	submitIssueItem := fyne.NewMenuItem("Submit issue", func() {
		openExternalURL("https://github.com/Rhizomatica/mercury/issues/new")
	})
	helpMenu := fyne.NewMenu("Help", versionItem, supportItem, mailingItem, submitIssueItem)

	logsItem := fyne.NewMenuItem("Open logs directory", openLogsDir)
	logsMenu := fyne.NewMenu("Logs", logsItem)

	myWindow.SetMainMenu(fyne.NewMainMenu(remoteMenu, configMenu, logsMenu, helpMenu))

	// Single idempotent teardown, used by both the window-close handler and the
	// signal handler.  It runs entirely OFF the GL/main thread: SetOnClosed is
	// invoked on the main thread, and mercury_engine_shutdown() joins the engine
	// threads (audio/modem) which takes a moment — doing that inline would block
	// the GL loop and leave the window half-closed, which some window managers
	// dislike.  Instead shutdown() returns immediately (just kicks a goroutine)
	// so the window closes at once, and teardown finishes in the background,
	// then os.Exit.  A watchdog force-exits if a join ever wedges (the UI's
	// equivalent of the daemon's alarm(10)).  mercuryStop() still runs first so
	// the engine unkeys the radio and flushes on the way out.
	var shutdownOnce sync.Once
	shutdown := func() {
		// Remember where and how large the window is so the next launch opens
		// in the same place. Read before teardown: the window is still alive
		// when SetOnClosed runs.
		saveWindowGeometry(myWindow, myApp.Preferences())
		shutdownOnce.Do(func() {
			go func() {
				go func() {
					time.Sleep(5 * time.Second)
					os.Exit(0)
				}()
				state.mu.Lock()
				link := state.link
				cancel := state.wsCancel
				state.link = nil
				state.wsCancel = nil
				state.wsConnected = false
				state.mu.Unlock()
				if link != nil {
					link.Close()
				}
				if cancel != nil {
					cancel()
				}
				mercuryStop()
				if uiLog != nil {
					_ = uiLog.Close()
				}
				os.Exit(0)
			}()
		})
	}

	myWindow.SetOnClosed(shutdown)

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sigCh
		// Call the teardown directly rather than routing through the GL event
		// loop, so Ctrl+C works even if the window/GL is wedged.
		shutdown()
	}()

	// Pick the starting mode now that the link helpers exist. Local when this
	// build carries an engine; otherwise the network is the only way to reach
	// one, so Remote is the honest default and its fields stay usable.
	if _, err := newEngineLink().probe(); err == nil {
		engineSelect.SetSelected("Local")
	} else {
		engineSelect.SetSelected("Remote")
	}

	go func() {
		time.Sleep(200 * time.Millisecond)

		defaultConfig := filepath.Join(getBaseDir(), "mercury.ini")

		logPath := filepath.Join(getLogDir(), "mercury_engine.log")
		appendLog("Starting Mercury engine...\n")
		// Forward the process args so the engine's own CLI parser applies them.
		if err := mercuryStart(defaultConfig, logPath, os.Args); err != nil {
			appendLog(fmt.Sprintf("Failed to start Mercury engine: %v\n", err))
			return
		}
		appendLog("Mercury engine started — connecting...\n")
		engineReady.Store(true)
		connectLink()
	}()

	myWindow.ShowAndRun()

	// ShowAndRun returns once the window has closed. Teardown now runs off the
	// main thread, so make sure it has been kicked off (idempotent — SetOnClosed
	// normally does) and then block: the teardown goroutine calls os.Exit when
	// done (or the watchdog does), so main() must not return here and terminate
	// the process before the engine has unkeyed the radio and stopped cleanly.
	shutdown()
	select {}
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
	sr := state.spectrumRate
	state.mu.RUnlock()
	if len(vals) == 0 {
		return
	}
	// Limit the drawn bins so the spectrum lines up with the waterfall's
	// 0..3000 Hz display. If the sample-rate's Nyquist exceeds 3 kHz,
	// scale the number of bins used accordingly.
	binsToUse := len(vals)
	if sr > 0 {
		nyq := float64(sr) / 2.0
		const maxDisplayHz = 3000.0
		if nyq > maxDisplayHz {
			scaled := int(float64(len(vals)) * (maxDisplayHz / nyq))
			if scaled >= 1 {
				binsToUse = scaled
			} else {
				binsToUse = 1
			}
		}
	}
	valsToDraw := vals[:binsToUse]

	ctx := &spectrumContext{img: img, w: w, h: h}
	ctx.drawGrid()
	// draw the spectrum line with a bold cyan color
	ctx.drawLine(valsToDraw)
	// draw a faint filled area under the line
	ctx.fillUnderLine(valsToDraw)
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
	palette := state.waterfallPalette
	sr := state.spectrumRate
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
		if len(row) == 0 {
			continue
		}
		// Determine how many bins to use so the waterfall shows 0..3000 Hz
		binsToUse := len(row)
		if sr > 0 {
			nyq := float64(sr) / 2.0
			const maxDisplayHz = 3000.0
			if nyq > maxDisplayHz {
				// scale down the number of bins to cover only up to maxDisplayHz
				scaled := int(float64(len(row)) * (maxDisplayHz / nyq))
				if scaled >= 1 {
					binsToUse = scaled
				} else {
					binsToUse = 1
				}
			}
		}
		for x := 0; x < w; x++ {
			srcIdx := (x * binsToUse) / w
			if srcIdx >= len(row) {
				srcIdx = len(row) - 1
			}
			v := float64(row[srcIdx])
			if math.IsNaN(v) {
				continue
			}
			c := waterfallColorForDB(v, palette)
			img.SetNRGBA(x, destY, c)
		}
	}
}

func waterfallColorForDB(v float64, palette string) color.NRGBA {
	t := (v + 70.0) / 70.0
	if t < 0 {
		t = 0
	}
	if t > 1 {
		t = 1
	}
	switch palette {
	case "turbo":
		return turboColor(t)
	case "hot":
		return hotColor(t)
	case "grayscale":
		return grayscaleColor(t)
	default:
		return blackblueColor(t)
	}
}

func turboColor(t float64) color.NRGBA {
	r := clamp(34.61+t*(1172.33+t*(-10793.56+t*(33300.12+t*(-38394.49+t*14825.05)))), 0, 255)
	g := clamp(23.31+t*(557.33+t*(1225.33+t*(-5765.73+t*(8240.07+t*(-3832.07))))), 0, 255)
	b := clamp(27.2+t*(3211.1+t*(-15327.97+t*(27814.0+t*(-22569.18+t*6838.66)))), 0, 255)
	return color.NRGBA{R: uint8(r), G: uint8(g), B: uint8(b), A: 0xFF}
}

func hotColor(t float64) color.NRGBA {
	r := clamp(t*3*255, 0, 255)
	g := clamp((t-0.33)*3*255, 0, 255)
	b := clamp((t-0.67)*3*255, 0, 255)
	return color.NRGBA{R: uint8(r), G: uint8(g), B: uint8(b), A: 0xFF}
}

func grayscaleColor(t float64) color.NRGBA {
	v := uint8(t * 255)
	return color.NRGBA{R: v, G: v, B: v, A: 0xFF}
}

func blackblueColor(t float64) color.NRGBA {
	switch {
	case t < 0.25:
		f := t / 0.25
		return color.NRGBA{R: 0x00, G: 0x00, B: uint8(0x20 + int(200*f)), A: 0xFF}
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

func getExeDir() string {
	if exePath, err := os.Executable(); err == nil {
		return filepath.Dir(exePath)
	}
	return "."
}

func getBaseDir() string {
	if runtime.GOOS == "windows" {
		return getExeDir()
	}
	return "."
}

func getLogDir() string {
	if runtime.GOOS == "linux" {
		if dir := os.Getenv("XDG_STATE_HOME"); dir != "" {
			return filepath.Join(dir, "mercury", "logs")
		}
		if home, err := os.UserHomeDir(); err == nil {
			return filepath.Join(home, ".local", "state", "mercury", "logs")
		}
		return "."
	}
	if runtime.GOOS == "windows" {
		return filepath.Join(getExeDir(), "logs")
	}
	return "."
}
