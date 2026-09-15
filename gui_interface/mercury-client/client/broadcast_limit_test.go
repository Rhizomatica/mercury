package client

import (
	"fmt"
	"testing"
)

// The limit exists to stop the TNC truncating a message on the air, so the
// test that matters is that a message AT the limit still fits the frame once
// SendBroadcast has wrapped it and Mercury has added its 3 bytes of framing.
// Checking the arithmetic against itself would prove nothing.
func TestBroadcastChatLimitFitsTheFrame(t *testing.T) {
	const mercuryFraming = 3

	for _, frameSize := range []int{14, 30, 54, 126, 510, 1180, 1213} {
		for _, call := range []string{"PU2UIT-3", "M0ABC", "VE7AAAAAAAAA-15"} {
			limit := BroadcastChatLimit(frameSize, call)
			if limit == 0 {
				continue // mode cannot carry chat from this callsign
			}

			msg := make([]byte, limit)
			for i := range msg {
				msg[i] = 'x'
			}
			// Exactly what SendBroadcast puts on the wire.
			payload := fmt.Sprintf("%s: %s\n", call, string(msg))

			if got := len(payload) + mercuryFraming; got != frameSize {
				t.Errorf("frame %d, callsign %q: a message at the limit produces %d bytes on the air, want %d",
					frameSize, call, got, frameSize)
			}
			// One more character must NOT fit.
			over := fmt.Sprintf("%s: %s\n", call, string(msg)+"x")
			if len(over)+mercuryFraming <= frameSize {
				t.Errorf("frame %d, callsign %q: limit %d is too conservative",
					frameSize, call, limit)
			}
		}
	}
}

// A frame too small for the callsign alone must report 0, not a negative
// length that a caller would then use as a slice bound.
func TestBroadcastChatLimitNeverNegative(t *testing.T) {
	if got := BroadcastChatLimit(14, "VE7AAAAAAAAA-15"); got != 0 {
		t.Errorf("got %d, want 0 for a frame that cannot hold the callsign", got)
	}
	if got := BroadcastChatLimit(0, "M0ABC"); got != 0 {
		t.Errorf("got %d, want 0 for a zero frame size", got)
	}
}

// The documented figures, so a change to the framing shows up as a test failure
// rather than a quietly different limit.
func TestBroadcastChatLimitDocumentedValues(t *testing.T) {
	for _, tc := range []struct {
		frame int
		call  string
		want  int
	}{
		{126, "PU2UIT-3", 112},   // DATAC3
		{54, "PU2UIT-3", 40},     // DATAC4
		{510, "PU2UIT-3", 496},   // DATAC1
		{1180, "PU2UIT-3", 1166}, // DATAC17
	} {
		if got := BroadcastChatLimit(tc.frame, tc.call); got != tc.want {
			t.Errorf("frame %d as %q: got %d, want %d", tc.frame, tc.call, got, tc.want)
		}
	}
}
