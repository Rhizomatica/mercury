package main

import (
	"testing"
	"time"
)

type fakeClock struct{ t time.Time }

func (c *fakeClock) now() time.Time { return c.t }

func newTestGain() (*txGainControl, *fakeClock) {
	c := &fakeClock{t: time.Unix(1000, 0)}
	return &txGainControl{now: c.now}, c
}

// A Fyne slider: SetValue fires OnChanged and OnChangeEnded; a user drag
// fires OnChanged on each step and OnChangeEnded only when the drag ends.
// sent records what reached the engine.
type fakeSlider struct {
	value float64
	g     *txGainControl
	sent  []float64
}

// set plays SetValue.
func (s *fakeSlider) set(v float64) {
	if v == s.value {
		return
	}
	s.value = v
	s.moved()
	s.ended()
}

// drag plays the user's pointer moves.
func (s *fakeSlider) drag(v float64) {
	if v == s.value {
		return
	}
	s.value = v
	s.moved()
}

// end plays DragEnd: the user lets go.
func (s *fakeSlider) end() { s.ended() }

// moved plays OnChanged: the label follows the user's finger.
func (s *fakeSlider) moved() {
	s.g.userGesture()
}

// ended plays OnChangeEnded: the value goes to the engine.
func (s *fakeSlider) ended() {
	if s.g.userGesture() {
		s.sent = append(s.sent, s.value)
	}
}

func (s *fakeSlider) status(db float64) {
	if s.g.engineGain(s.value, db) {
		s.g.setSlider(s.set, db)
	}
}

// N3JXB, 2026-11-22: the engine started at -9.5 dB from mercury.ini.  The
// slider takes that value from the first status, and sends nothing for it.
func TestTxGainSliderTakesTheEngineValueWithoutSendingIt(t *testing.T) {
	g, _ := newTestGain()
	s := &fakeSlider{g: g}
	s.status(-9.5)
	if s.value != -9.5 {
		t.Fatalf("slider at %.1f, want -9.5", s.value)
	}
	if len(s.sent) != 0 {
		t.Fatalf("the engine's own value was sent back: %v", s.sent)
	}
	// ...so the user's first move starts from there, not from 0 dB.
	s.drag(-9.0)
	s.end()
	if len(s.sent) != 1 || s.sent[0] != -9.0 {
		t.Fatalf("sent %v, want [-9]", s.sent)
	}
}

// A drag across steps sends set_tx_gain once, when it ends, so the engine
// does not rewrite mercury.ini for every 0.5 dB step.
func TestTxGainDragSendsOnceAtTheEnd(t *testing.T) {
	g, _ := newTestGain()
	s := &fakeSlider{g: g}
	s.status(-9.5)
	s.drag(-9.0)
	s.drag(-8.0)
	s.drag(-6.5)
	if len(s.sent) != 0 {
		t.Fatalf("a drag step was sent before the end: %v", s.sent)
	}
	s.end()
	if len(s.sent) != 1 || s.sent[0] != -6.5 {
		t.Fatalf("sent %v, want [-6.5]", s.sent)
	}
}

// At 0 dB too: the first status marks the slider synced even when nothing moves.
func TestTxGainFirstStatusAtZeroSyncs(t *testing.T) {
	g, _ := newTestGain()
	s := &fakeSlider{g: g}
	s.status(0)
	if !g.synced || len(s.sent) != 0 {
		t.Fatalf("synced=%v sent=%v", g.synced, s.sent)
	}
}

// A gain the 0.5 dB slider cannot show exactly is not set again every status.
func TestTxGainOffStepValueIsSetOnce(t *testing.T) {
	g, _ := newTestGain()
	s := &fakeSlider{g: g}
	s.status(-4.3)
	s.value = -4.5 // Fyne snaps it to the step
	if g.engineGain(s.value, -4.3) {
		t.Fatal("-4.3 dB would be set again on every status")
	}
}

// While the user drags, a status still carrying the old gain does not move the
// slider back; once the hold has passed, the engine's value wins again.
func TestTxGainStatusDoesNotFightADrag(t *testing.T) {
	g, clk := newTestGain()
	s := &fakeSlider{g: g}
	s.status(-9.5)
	s.drag(-6.0)
	s.end() // the drag ends: -6.0 goes to the engine once
	if len(s.sent) != 1 || s.sent[0] != -6.0 {
		t.Fatalf("sent %v, want [-6]", s.sent)
	}
	clk.t = clk.t.Add(500 * time.Millisecond)
	s.status(-9.5) // the report from before set_tx_gain landed
	if s.value != -6.0 {
		t.Fatalf("a stale status moved the slider to %.1f mid-drag", s.value)
	}
	if !g.userActive() {
		t.Fatal("the label would follow the stale status")
	}
	clk.t = clk.t.Add(txGainHold)
	s.status(-6.0) // the engine now reports the user's value: nothing to do
	if s.value != -6.0 || len(s.sent) != 1 {
		t.Fatalf("value %.1f sent %v", s.value, s.sent)
	}
	s.status(-3.0) // changed elsewhere (the web UI, another client): followed
	if s.value != -3.0 || len(s.sent) != 1 {
		t.Fatalf("value %.1f sent %v, want -3 and nothing more sent", s.value, s.sent)
	}
}
