package integration

import (
	"context"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"syscall"
	"testing"
	"time"
)

const (
	kissFEND  = 0xC0
	kissFESC  = 0xDB
	kissTFEND = 0xDC
	kissTFESC = 0xDD
	// CMD_MODEM_FRAME: "exactly one modem frame, transmit it untouched" --
	// what hermes-broadcast's broadcast_daemon sends.
	kissCmdModemFrame = 0x03
	// PACKET_TYPE_BROADCAST_DATA << PACKET_TYPE_SHIFT, extension 0.
	bcastDataHeader = 0x04 << 5
)

func kissEncode(cmd byte, payload []byte) []byte {
	out := []byte{kissFEND, cmd}
	for _, b := range payload {
		switch b {
		case kissFEND:
			out = append(out, kissFESC, kissTFEND)
		case kissFESC:
			out = append(out, kissFESC, kissTFESC)
		default:
			out = append(out, b)
		}
	}
	return append(out, kissFEND)
}

// kissFrames reads KISS frames off conn and sends each unescaped payload
// (command byte stripped) on the channel until conn closes.
func kissFrames(conn net.Conn, frames chan<- []byte) {
	buf := make([]byte, 4096)
	var cur []byte
	in, esc := false, false
	for {
		n, err := conn.Read(buf)
		for _, b := range buf[:n] {
			switch {
			case b == kissFEND:
				if in && len(cur) > 1 {
					frames <- append([]byte(nil), cur[1:]...)
				}
				cur, in, esc = cur[:0], true, false
			case !in:
			case esc:
				if b == kissTFEND {
					b = kissFEND
				} else if b == kissTFESC {
					b = kissFESC
				}
				cur, esc = append(cur, b), false
			case b == kissFESC:
				esc = true
			default:
				cur = append(cur, b)
			}
		}
		if err != nil {
			close(frames)
			return
		}
	}
}

// A carousel must keep going out when the ARQ data port receives bytes while
// no session is up.
//
// On air (1.9.15, sbitx gateway) an ARQ client wrote 32 bytes ~50 s after its
// session had ended.  The TX thread counted them as ARQ traffic, pinned the
// modem to the ARQ control mode (DATAC16, 14-byte frames), and broadcast
// frames -- sent only at the listen mode's frame size -- never went out again:
// a 352-frame carousel stopped after 12 frames for 80 minutes, and with the
// queue full the broadcast port stopped being read.
func TestBroadcastSurvivesStrayARQData(t *testing.T) {
	repoRoot := mustRepoRoot(t)
	bin := locateOrBuildMercury(t, repoRoot)
	chBin, err := buildCh(repoRoot)
	if err != nil {
		t.Skipf("channel simulator unavailable: %v", err)
	}

	const mode, frameSize, nFrames = 1, 126, 5 // DATAC3

	dir := t.TempDir()
	aRX := filepath.Join(dir, "a_rx.fifo")
	aTX := filepath.Join(dir, "a_tx.fifo")
	bRX := filepath.Join(dir, "b_rx.fifo")
	bTX := filepath.Join(dir, "b_tx.fifo")
	for _, p := range []string{aRX, aTX, bRX, bTX} {
		if err := syscall.Mkfifo(p, 0600); err != nil {
			t.Fatalf("mkfifo %s: %v", p, err)
		}
	}
	aPort, bPort := freePortPair(t), freePortPair(t)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	start := func(name, rx, tx string, port int) (*exec.Cmd, *processWait, *os.File, *os.File) {
		stdout, stderr := tempLogFilesNamed(t, name)
		cmd := exec.CommandContext(ctx, bin,
			"-x", "fifo", "-i", rx, "-o", tx,
			"-p", fmt.Sprint(port), "-b", fmt.Sprint(port+100),
			"-m", fmt.Sprint(mode), "-v",
			"-C", filepath.Join(t.TempDir(), "missing-mercury.ini"))
		cmd.Dir = repoRoot
		cmd.Stdout, cmd.Stderr = stdout, stderr
		if err := startChild(cmd); err != nil {
			t.Fatalf("start mercury %s: %v", name, err)
		}
		return cmd, waitForProcess(cmd), stdout, stderr
	}
	cmdA, procA, outA, errA := start("A", aRX, aTX, aPort)
	defer func() { _ = stopProcess(t, cmdA, procA, outA.Name(), errA.Name()) }()
	cmdB, procB, outB, errB := start("B", bRX, bTX, bPort)
	defer func() { _ = stopProcess(t, cmdB, procB, outB.Name(), errB.Name()) }()

	bridge := startChannelBridge(ctx, chBin, aTX, bRX, bTX, aRX, DefaultChannelParams())
	defer bridge.Close()

	fail := func(format string, args ...interface{}) {
		printLogs(t, outA.Name(), errA.Name())
		printLogs(t, outB.Name(), errB.Name())
		t.Fatalf(format, args...)
	}

	ctlA, err := waitForTCP(ctx, "127.0.0.1", aPort, controlPortTimeout, procA)
	if err != nil {
		fail("A control port: %v", err)
	}
	defer ctlA.Close()
	if _, err := waitForTCP(ctx, "127.0.0.1", bPort, controlPortTimeout, procB); err != nil {
		fail("B control port: %v", err)
	}
	dataA, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", aPort+1), 5*time.Second)
	if err != nil {
		fail("A data port: %v", err)
	}
	defer dataA.Close()
	bcastA, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", aPort+100), 5*time.Second)
	if err != nil {
		fail("A broadcast port: %v", err)
	}
	defer bcastA.Close()
	bcastB, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", bPort+100), 5*time.Second)
	if err != nil {
		fail("B broadcast port: %v", err)
	}
	defer bcastB.Close()

	got := make(chan []byte, 64)
	go kissFrames(bcastB, got)

	// The whole carousel in one write, as broadcast_daemon does.
	var carousel []byte
	for i := 0; i < nFrames; i++ {
		f := make([]byte, frameSize)
		f[0] = bcastDataHeader
		f[1] = byte(i)
		for j := 2; j < frameSize; j++ {
			f[j] = byte(i*31 + j)
		}
		carousel = append(carousel, kissEncode(kissCmdModemFrame, f)...)
	}
	if _, err := bcastA.Write(carousel); err != nil {
		fail("write carousel: %v", err)
	}

	seen := map[byte]bool{}
	stray := false
	deadline := time.After(time.Duration(nFrames*4+60) * time.Second)
	for len(seen) < nFrames {
		select {
		case f, ok := <-got:
			if !ok {
				fail("B broadcast port closed after %d/%d frames", len(seen), nFrames)
			}
			if len(f) >= 2 && f[0] == bcastDataHeader {
				seen[f[1]] = true
				t.Logf("B received broadcast frame %d (%d/%d)", f[1], len(seen), nFrames)
			}
			// Mid-carousel, no session up: a client's late write.
			if !stray {
				stray = true
				if _, err := dataA.Write([]byte("late bytes, no session up here!!")); err != nil {
					fail("stray ARQ write: %v", err)
				}
			}
		case <-deadline:
			fail("carousel stalled: B received %d/%d frames after a stray ARQ write", len(seen), nFrames)
		}
	}
}
