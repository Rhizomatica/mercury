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
}

const (
	engineStatusInterval   = 500 * time.Millisecond
	engineSpectrumInterval = 50 * time.Millisecond
	// Matches MODEM_STATS_NSPEC; the bridge clamps to its own size anyway.
	engineSpectrumBins = 512
)

func newEngineLink() *engineLink {
	return &engineLink{
		statusEvery:   engineStatusInterval,
		spectrumEvery: engineSpectrumInterval,
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

		// Reused across polls: the bridge copies into it, so one allocation
		// serves the whole session for status.
		var cst C.ui_status_t

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
				bins, rate, ok := pollSpectrum()
				if !ok {
					continue
				}
				emit(ctx, events, SpectrumEvent{Bins: bins, SampleRate: rate})
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
	defer func() {
		C.free(unsafe.Pointer(cName))
		C.free(unsafe.Pointer(cV1))
		C.free(unsafe.Pointer(cV2))
		C.free(unsafe.Pointer(cV3))
	}()

	if rc := C.mercury_ui_command(cName, cV1, cV2, cV3); rc != 0 {
		return fmt.Errorf("engine rejected command %q (rc=%d)", cmd.Name, int(rc))
	}
	return nil
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
	}
}

// pollSpectrum copies one FFT frame out of the engine. A fresh slice per frame
// because the waterfall keeps the rows.
func pollSpectrum() ([]float32, int, bool) {
	buf := make([]float32, engineSpectrumBins)
	var rate C.int
	n := C.mercury_ui_get_spectrum((*C.float)(unsafe.Pointer(&buf[0])),
		C.int(len(buf)), &rate)
	if n <= 0 {
		return nil, 0, false
	}
	return buf[:int(n)], int(rate), true
}
