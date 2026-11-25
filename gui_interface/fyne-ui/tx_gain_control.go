package main

import (
	"math"
	"time"
)

// txGainHold is how long the user's last move of the TX gain slider stands
// against telemetry.  The engine reports its gain in every status (about every
// 500 ms), and the report after a drag can still carry the value from before
// the set_tx_gain that changed it: followed at once, the label and the slider
// jumped back mid-drag.
const txGainHold = 1500 * time.Millisecond

// txGainControl keeps the TX gain slider on the engine's actual gain.
//
// The slider used to start at 0 dB and never learn the engine's value, which
// the engine reads from mercury.ini.  Only the label followed it.  The first
// touch then sent the slider's position: on air (N3JXB, 2026-11-22) the engine
// started at -9.5 dB and the first move sent +0.5 dB, 10 dB louder.
//
// Setting a Fyne slider's value fires its OnChanged and OnChangeEnded, so
// the engine's value written to the slider is marked (syncing) and not
// sent back as the user's.
type txGainControl struct {
	now     func() time.Time
	syncing bool      // the engine's value is being written to the slider
	synced  bool      // the slider has shown the engine's value at least once
	userAt  time.Time // the user's last move of the slider
}

func newTxGainControl() *txGainControl { return &txGainControl{now: time.Now} }

// userGesture is the slider's OnChanged and OnChangeEnded.  It marks the
// user's move, which keeps telemetry off the slider and its label for
// txGainHold, and reports whether the callback came from the user rather
// than from setSlider: Fyne fires both callbacks on SetValue.
func (g *txGainControl) userGesture() bool {
	if g.syncing {
		return false
	}
	g.userAt = g.now()
	return true
}

// userActive reports whether the user moved the slider within txGainHold:
// while so, telemetry moves neither the slider nor its label.
func (g *txGainControl) userActive() bool {
	return !g.userAt.IsZero() && g.now().Sub(g.userAt) < txGainHold
}

// engineGain is a status's TX gain.  It reports whether the slider, now at
// slider, should be set to db; the caller then sets it through setSlider.
func (g *txGainControl) engineGain(slider, db float64) bool {
	if g.userActive() {
		return false
	}
	// Within half the slider's 0.5 dB step it already shows db as near as it
	// can (mercury.ini may hold -4.3): set again, it would only snap back.
	return !g.synced || math.Abs(slider-db) >= 0.25
}

// setSlider writes the engine's value with OnChanged's send suppressed.
func (g *txGainControl) setSlider(set func(float64), db float64) {
	g.syncing = true
	set(db)
	g.syncing = false
	g.synced = true
}
