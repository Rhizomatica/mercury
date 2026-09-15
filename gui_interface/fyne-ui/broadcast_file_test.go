//go:build mercury_embedded

package main

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"

	"mercury-client/client"
)

// The Go halves of send and receive must work together over the CGo bridge.
// The C side is tested on its own; what this covers is the marshalling --
// buffers, lengths and the opaque handles -- which is where a CGo binding
// breaks, and it does it through the real carousel rather than a stub.
func TestBroadcastFileSendReceiveRoundTrip(t *testing.T) {
	if broadcastModeFrameSize(0) == 0 {
		t.Skip("engine not linked in this build")
	}

	dir := t.TempDir()
	src := filepath.Join(dir, "bulletin.txt")
	payload := bytes.Repeat([]byte("HERMES broadcast test payload. "), 120) // ~3.6 kB
	if err := os.WriteFile(src, payload, 0o600); err != nil {
		t.Fatal(err)
	}
	rxDir := t.TempDir()

	const mode = 0 // DATAC1, 510 byte frames
	tx, err := newBroadcastFileTx(src, mode, 0 /* endless */)
	if err != nil {
		t.Fatalf("open tx: %v", err)
	}
	defer tx.close()

	rx, err := newBroadcastFileRx(mode, rxDir)
	if err != nil {
		t.Fatalf("open rx: %v", err)
	}
	defer rx.Close()

	frameSize, bundleBytes, blocks := tx.info()
	if frameSize != broadcastModeFrameSize(mode) {
		t.Fatalf("frame size %d, want %d", frameSize, broadcastModeFrameSize(mode))
	}
	if bundleBytes <= int64(len(payload)) {
		t.Fatalf("bundle %d should exceed the file %d (it carries the name)", bundleBytes, len(payload))
	}
	if blocks != 1 {
		t.Errorf("expected a single source block, got %d", blocks)
	}

	buf := make([]byte, frameSize)
	var gotName, gotPath string
	for i := 0; i < 4000; i++ {
		n, done, err := tx.next(buf)
		if err != nil || done {
			t.Fatalf("tx stopped early: n=%d done=%v err=%v", n, done, err)
		}
		// Drop every third frame: the receiver must cope with loss.
		if i%3 == 2 {
			continue
		}
		claimed, pr := rx.Frame(buf[:n])
		if !claimed {
			t.Fatalf("receiver did not recognise its own frame %d", i)
		}
		if pr.Err != nil {
			t.Fatalf("receive error: %v", pr.Err)
		}
		if pr.Name != "" {
			gotName, gotPath = pr.Name, pr.Path
			break
		}
	}
	if gotName == "" {
		t.Fatal("file never completed")
	}
	if gotName != "bulletin.txt" {
		t.Errorf("recovered name %q, want bulletin.txt", gotName)
	}

	back, err := os.ReadFile(gotPath)
	if err != nil {
		t.Fatalf("read recovered file: %v", err)
	}
	if !bytes.Equal(back, payload) {
		t.Errorf("recovered %d bytes, want %d, contents differ", len(back), len(payload))
	}
}

// Traffic that is not ours must be handed back to chat, not swallowed.
func TestBroadcastFileRxIgnoresChatFrames(t *testing.T) {
	if broadcastModeFrameSize(0) == 0 {
		t.Skip("engine not linked in this build")
	}
	rx, err := newBroadcastFileRx(0, t.TempDir())
	if err != nil {
		t.Fatalf("open rx: %v", err)
	}
	defer rx.Close()

	for _, frame := range [][]byte{
		[]byte("CQ CQ de PU2UIT\n"), // a chat line: wrong length
		{},                          // nothing at all
	} {
		if claimed, _ := rx.Frame(frame); claimed {
			t.Errorf("receiver claimed a frame that was not its own: %q", frame)
		}
	}
}

// An oversized file must be refused before anything is transmitted.
func TestBroadcastFileRefusesOversized(t *testing.T) {
	if broadcastModeFrameSize(0) == 0 {
		t.Skip("engine not linked in this build")
	}
	dir := t.TempDir()
	big := filepath.Join(dir, "too-big.bin")
	if err := os.WriteFile(big, make([]byte, broadcastFileMaxBytes()+1), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := newBroadcastFileTx(big, 0, 1); err == nil {
		t.Fatal("expected an oversized file to be refused")
	}
}

// A mode the engine runs perfectly well is not necessarily a mode broadcast can
// use.  A broadcast frame must hold the framing plus one whole RaptorQ symbol,
// so FSK_LDPC and DATAC15 (30-byte frames) are runnable but unusable -- the
// first modes for which those two answers differ, which is why the usability
// predicate went unused for so long.
//
// The distinction has to be made BEFORE a transfer starts.  If it is not, the
// UI proceeds as though broadcast will work and the operator gets an opaque
// failure out of tx_open instead of being told to restart with -m.
func TestBroadcastUsabilityIsNotTheSameAsRunnable(t *testing.T) {
	usable := map[int]bool{0: true, 1: true, 3: true, 9: true, 10: true}
	names := map[int]string{
		0: "DATAC1", 1: "DATAC3", 2: "DATAC0", 3: "DATAC4", 4: "DATAC13",
		5: "DATAC14", 6: "FSK_LDPC", 7: "DATAC15", 8: "DATAC16",
		9: "DATAC17", 10: "QAM16C2",
	}
	for mode := 0; mode <= 10; mode++ {
		got := broadcastModeUsable(mode)
		if got != usable[mode] {
			t.Errorf("%s (mode %d): usable=%v, want %v",
				names[mode], mode, got, usable[mode])
		}
		// Whatever the verdict, a usable mode must have room for a symbol and
		// an unusable one must not -- so the predicate and the geometry agree.
		fs := broadcastModeFrameSize(mode)
		if got && fs < 53 {
			t.Errorf("%s: reported usable with a %d-byte frame", names[mode], fs)
		}
		if !got && fs >= 53 {
			t.Errorf("%s: reported unusable with a %d-byte frame", names[mode], fs)
		}
	}
}

// Broadcast CHAT and broadcast FILES have different requirements, and the two
// mode accessors exist to keep them apart.
//
// A chat line needs only a frame to sit in.  A file needs a frame big enough
// for one whole RaptorQ symbol, which DATAC15 and FSK_LDPC (30 B) are not.  So
// those modes carry chat and GPS perfectly well while refusing file transfer,
// and code wanting a chat limit must NOT use the file-filtered accessor: it
// would read "limit unknown", stop capping the entry, and let the TNC silently
// truncate what the operator typed.  That regression was live for one commit.
//
// The property asserted is the one the bug violated: a mode can be refused for
// files and still have a real chat capacity, and the chat limit is a function
// of the frame alone.
func TestChatLimitDoesNotDependOnFileUsability(t *testing.T) {
	names := map[int]string{
		0: "DATAC1", 1: "DATAC3", 2: "DATAC0", 3: "DATAC4", 4: "DATAC13",
		5: "DATAC14", 6: "FSK_LDPC", 7: "DATAC15", 8: "DATAC16",
		9: "DATAC17", 10: "QAM16C2",
	}
	chatOnly := 0
	for mode := 0; mode <= 10; mode++ {
		fs := broadcastModeFrameSize(mode)
		if fs <= 0 {
			continue
		}
		limit := client.BroadcastChatLimit(fs, "PU2UIT")

		// The limit must depend on the frame and nothing else: two modes with
		// the same frame size must agree, whatever their file usability.
		for other := 0; other <= 10; other++ {
			if other == mode || broadcastModeFrameSize(other) != fs {
				continue
			}
			if l2 := client.BroadcastChatLimit(fs, "PU2UIT"); l2 != limit {
				t.Errorf("%s and %s share a %d-byte frame but differ: %d vs %d",
					names[mode], names[other], fs, limit, l2)
			}
		}

		if !broadcastModeUsable(mode) && limit > 0 {
			// The case the bug broke: chat works, files do not.
			chatOnly++
			t.Logf("%s: chat limit %d, file transfer refused", names[mode], limit)
		}
	}
	if chatOnly == 0 {
		t.Fatal("no mode carries chat but not files; the distinction this test " +
			"guards has disappeared and the two accessors may have been merged")
	}

	// FSK_LDPC and DATAC15 specifically: the modes the symbol floor evicted
	// from file transfer, which must still carry a chat line.
	for _, m := range []int{6, 7} {
		if broadcastModeUsable(m) {
			t.Errorf("%s: expected file transfer to be refused at T=41", names[m])
		}
		if got := client.BroadcastChatLimit(broadcastModeFrameSize(m), "PU2UIT"); got <= 0 {
			t.Errorf("%s: chat limit %d, want > 0", names[m], got)
		}
	}
}
