//go:build mercury_embedded

package main

/*
#cgo CFLAGS: -I${SRCDIR}/../.. -I${SRCDIR}/../../modem/freedv -I${SRCDIR}/../../modem -I${SRCDIR}/../../datalink_broadcast -I${SRCDIR}/../../data_interfaces -I${SRCDIR}/../../datalink_arq -I${SRCDIR}/../../audioio/ffaudio -I${SRCDIR}/../../common -I${SRCDIR}/../../gui_interface -I${SRCDIR}/../../radio_io -I${SRCDIR}/../../common/iniparser -I${SRCDIR}/engine -pthread -D_GNU_SOURCE

#include <stdlib.h>
#include "mercury_bridge.h"
*/
import "C"

import (
	"context"
	"fmt"
	"time"
	"unsafe"
)

// engineLink drives the engine linked into this binary. State is pulled from
// the engine on a ticker rather than pushed through a callback: the modem's
// publisher threads must never end up blocked in Go code, and pulling keeps
// the cadence tied to what the UI can actually draw.
//
// The intervals match what the engine publishes over the websocket, so the
// embedded UI and a remote one see the same picture at the same rate.
type engineLink struct {
	statusEvery   time.Duration
	spectrumEvery time.Duration

	// Signalled after a command that changes what the pickers should show, so
	// the lists are re-read instead of going stale.
	refresh chan struct{}
}

const (
	engineStatusInterval = 500 * time.Millisecond
	// The engine produces FFT frames at ~20 Hz on its own clock. Polling at
	// the same rate on a different clock aliases: some ticks see the frame
	// twice, others miss one, and the waterfall stutters however fast the
	// machine is. Poll faster and let the sequence number decide, so each
	// frame is taken exactly once, promptly.
	engineSpectrumInterval = 20 * time.Millisecond
	// Matches MODEM_STATS_NSPEC; the bridge clamps to its own size anyway.
	engineSpectrumBins = 512
	// After a config change that restarts a subsystem (audioio, hamlib),
	// re-read the device lists several times so a slow restart is not
	// reported as the stale pre-change selection.
	refreshRetryInterval = 200 * time.Millisecond
	refreshRetries       = 5
)

func newEngineLink() *engineLink {
	return &engineLink{
		statusEvery:   engineStatusInterval,
		spectrumEvery: engineSpectrumInterval,
		refresh:       make(chan struct{}, 1),
	}
}

func (l *engineLink) Name() string { return "embedded engine" }

// probe reports whether the engine is actually linked into this binary. With
// the build tag on it always is, so this only ever fails in the stub.
func (l *engineLink) probe() (bool, error) { return true, nil }

func (l *engineLink) Start(ctx context.Context) (<-chan Event, error) {
	events := make(chan Event, 64)

	go func() {
		defer close(events)

		statusTick := time.NewTicker(l.statusEvery)
		specTick := time.NewTicker(l.spectrumEvery)
		defer statusTick.Stop()
		defer specTick.Stop()

		emit(ctx, events, LinkStateEvent{Up: true, Detail: "Engine running in-process (no socket)."})

		// The websocket path pushes the pickers when a client connects; here
		// there is no connect event, so read them once up front.
		listsOK := l.emitDeviceLists(ctx, events)

		// If the engine was not ready yet the lists come back empty. Retry for
		// a few seconds rather than leaving the pickers on "(Select one)" for
		// the rest of the session -- enumeration is slower on a Pi, and this is
		// the only chance the local path gets.
		listRetry := time.NewTicker(500 * time.Millisecond)
		defer listRetry.Stop()
		listRetriesLeft := 20

		// Reused across polls: the bridge copies into it, so one allocation
		// serves the whole session for status.
		var cst C.ui_status_t

		var lastSpectrumSeq uint64

		for {
			select {
			case <-ctx.Done():
				return

			case <-statusTick.C:
				if C.mercury_ui_get_status(&cst) == 0 {
					continue // engine has not published its first snapshot yet
				}
				emit(ctx, events, StatusEvent{Status: statusFromC(&cst)})

			case <-specTick.C:
				bins, rate, seq, ok := pollSpectrum()
				if !ok || seq == lastSpectrumSeq {
					continue // nothing new since the last poll
				}
				lastSpectrumSeq = seq
				emit(ctx, events, SpectrumEvent{Bins: bins, SampleRate: rate})

			case <-listRetry.C:
				if listsOK || listRetriesLeft == 0 {
					listRetry.Stop()
					continue
				}
				listRetriesLeft--
				listsOK = l.emitDeviceLists(ctx, events)

			case <-l.refresh:
				// A device or radio change was just applied; re-read so the
				// pickers show what the engine actually ended up using, which is
				// not always what was asked for.  Retry a few times because the
				// restart it triggered may take longer than one read to settle.
				for i := 0; i <= refreshRetries; i++ {
					_ = l.emitDeviceLists(ctx, events)
					if i == refreshRetries {
						break
					}
					select {
					case <-ctx.Done():
						return
					case <-time.After(refreshRetryInterval):
					}
				}
			}
		}
	}()

	return events, nil
}

func (l *engineLink) Send(cmd Command) error {
	cName := C.CString(cmd.Name)
	cV1 := C.CString(cmd.Value)
	cV2 := C.CString(cmd.Value2)
	cV3 := C.CString(cmd.Value3)
	cV4 := C.CString(cmd.Value4)
	defer func() {
		C.free(unsafe.Pointer(cName))
		C.free(unsafe.Pointer(cV1))
		C.free(unsafe.Pointer(cV2))
		C.free(unsafe.Pointer(cV3))
		C.free(unsafe.Pointer(cV4))
	}()

	if rc := C.mercury_ui_command(cName, cV1, cV2, cV3, cV4); rc != 0 {
		return fmt.Errorf("engine rejected command %q (rc=%d)", cmd.Name, int(rc))
	}

	switch cmd.Name {
	case "set_audio_config", "set_radio_config", "set_ptt_config":
		// The Start goroutine owns the retry timing (cancellable via ctx),
		// so no bare time.AfterFunc here that would outlive Close().
		select {
		case l.refresh <- struct{}{}:
		default:
		}
	}
	return nil
}

// emitDeviceLists reads the audio, channel and radio pickers out of the engine
// and publishes them as the same events the websocket path produces.
func (l *engineLink) emitDeviceLists(ctx context.Context, events chan<- Event) bool {
	gotAudio, gotRadio := false, false
	if items, selected, ok := readAudioDevices(C.UI_DEV_CAPTURE); ok {
		gotAudio = true
		emit(ctx, events, DeviceListEvent{Kind: DeviceCapture, Items: items, Selected: selected})
	}
	if items, selected, ok := readAudioDevices(C.UI_DEV_PLAYBACK); ok {
		emit(ctx, events, DeviceListEvent{Kind: DevicePlayback, Items: items, Selected: selected})
	}

	// The channel list is fixed; only the selection comes from the engine.
	channels := []optionItem{
		{Name: "left", ID: "left"},
		{Name: "right", ID: "right"},
		{Name: "stereo", ID: "stereo"},
	}
	selectedChannel := "left"
	switch int(C.mercury_ui_get_input_channel()) {
	case 1:
		selectedChannel = "right"
	case 2:
		selectedChannel = "stereo"
	}
	emit(ctx, events, DeviceListEvent{
		Kind: DeviceInputChannel, Items: channels, Selected: selectedChannel,
	})

	if ev, ok := readRadioList(); ok {
		gotRadio = true
		emit(ctx, events, ev)
	}
	return gotAudio && gotRadio
}

// maxAudioDevices matches the engine-side cap in ui_comm_get_audio_devices().
const maxAudioDevices = 32

// maxRadios covers hamlib's catalogue plus the leading "None".
const maxRadios = 513

func readAudioDevices(kind C.ui_device_kind_t) ([]optionItem, string, bool) {
	devs := make([]C.ui_device_t, maxAudioDevices)
	// Sized from the engine's own constant: a device id that does not fit here
	// is silently truncated, which is how issue #185 lost a PulseAudio node
	// name and left the modem bound to the default card.
	sel := make([]C.char, C.UI_DEV_ID_MAX)

	n := C.mercury_ui_get_audio_devices(C.int(kind), &devs[0], C.int(len(devs)),
		&sel[0], C.int(len(sel)))
	if n <= 0 {
		return nil, "", false
	}

	items := make([]optionItem, 0, int(n))
	for i := 0; i < int(n); i++ {
		items = append(items, optionItem{
			ID:   C.GoString(&devs[i].id[0]),
			Name: C.GoString(&devs[i].name[0]),
		})
	}
	return items, C.GoString(&sel[0]), true
}

func readRadioList() (RadioListEvent, bool) {
	devs := make([]C.ui_device_t, maxRadios)
	sel := make([]C.char, 16)
	devPath := make([]C.char, 512)
	method := make([]C.char, 32)
	var speed C.int

	n := C.mercury_ui_get_radio_list(&devs[0], C.int(len(devs)),
		&sel[0], C.int(len(sel)),
		&devPath[0], C.int(len(devPath)), &speed,
		&method[0], C.int(len(method)))
	if n <= 0 {
		return RadioListEvent{}, false
	}

	items := make([]optionItem, 0, int(n))
	for i := 0; i < int(n); i++ {
		items = append(items, optionItem{
			ID:   C.GoString(&devs[i].id[0]),
			Name: C.GoString(&devs[i].name[0]),
		})
	}
	// "None" already leads the list; the rest keep hamlib order, matching what
	// the websocket path sends before the UI sorts it.
	sortRadioItems(items)

	serial := ""
	if speed > 0 {
		serial = fmt.Sprintf("%d", int(speed))
	}
	return RadioListEvent{
		Items:       items,
		Selected:    C.GoString(&sel[0]),
		DevicePath:  C.GoString(&devPath[0]),
		SerialSpeed: serial,
		PTTMethod:   C.GoString(&method[0]),
	}, true
}

func (l *engineLink) SetWaterfall(enabled bool) {
	cEn := C.bool(enabled)
	C.mercury_ui_set_waterfall(cEn)
}

// TCPPorts returns the ARQ base and broadcast TCP ports the engine is
// actually listening on (from its config).
func (l *engineLink) TCPPorts() (arqBase, broadcast int) {
	var a, b C.int
	C.mercury_ui_get_tcp_ports(&a, &b)
	return int(a), int(b)
}

// Version returns the engine's release version and git hash.
func (l *engineLink) Version() (version, gitHash string) {
	cv := make([]byte, 64)
	cg := make([]byte, 64)
	C.mercury_ui_get_version((*C.char)(unsafe.Pointer(&cv[0])), C.int(len(cv)),
		(*C.char)(unsafe.Pointer(&cg[0])), C.int(len(cg)))
	return cString(cv), cString(cg)
}

// cString converts a C buffer to a Go string, truncating at the first NUL.
func cString(b []byte) string {
	for i, c := range b {
		if c == 0 {
			return string(b[:i])
		}
	}
	return string(b)
}

func (l *engineLink) Close() {}

// statusFromC converts the engine's status struct into the UI's own type. This
// is the only place that knows both, and it is the counterpart of
// ui_status_to_json() on the C side — both render the same gathered snapshot.
func statusFromC(cst *C.ui_status_t) telemetryState {
	direction := "rx"
	if bool(cst.transmitting) {
		direction = "tx"
	}
	return telemetryState{
		Bitrate:            int(cst.bitrate_bps),
		SNR:                float64(cst.snr_db),
		UserCallsign:       C.GoString(&cst.user_callsign[0]),
		DestCallsign:       C.GoString(&cst.dest_callsign[0]),
		Sync:               bool(cst.sync),
		Direction:          direction,
		ClientTCPConnected: bool(cst.client_tcp_connected),
		BytesTransmitted:   int64(cst.bytes_transmitted),
		BytesReceived:      int64(cst.bytes_received),
		TXGainDB:           float64(cst.tx_gain_db),
		TXPeakDBFS:         float64(cst.tx_peak_dbfs),
		Waterfall:          bool(cst.waterfall_enabled),
		AudioOk:            bool(cst.audio_ok),
		AudioError:         C.GoString(&cst.audio_error[0]),
	}
}

// pollSpectrum copies one FFT frame out of the engine, with the sequence number
// that says whether it is new. A fresh slice per frame because the waterfall
// keeps the rows.
func pollSpectrum() ([]float32, int, uint64, bool) {
	buf := make([]float32, engineSpectrumBins)
	var rate C.int
	var seq C.ulonglong
	n := C.mercury_ui_get_spectrum((*C.float)(unsafe.Pointer(&buf[0])),
		C.int(len(buf)), &rate, &seq)
	if n <= 0 {
		return nil, 0, 0, false
	}
	return buf[:int(n)], int(rate), uint64(seq), true
}
