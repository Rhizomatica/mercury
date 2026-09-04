//go:build mercury_embedded

package main

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
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
