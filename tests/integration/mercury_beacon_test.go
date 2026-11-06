package integration

import (
	"bufio"
	"context"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
	"time"
)

// A station message (the HERMES beacon, packet type 0x07) goes on the mode
// BEACONMODE names, not the listen mode, and is heard at any listen mode by a
// decoder that already runs: DATAC16 by the control decoder, MFSK and MFSK500
// by the two call listeners of a station that is listening.  Both stations sit on DATAC3, so
// neither beacon could pass the broadcast plane's listen-mode size gate.
func TestBeaconHeardAtAnyListenMode(t *testing.T) {
	repoRoot := mustRepoRoot(t)
	bin := locateOrBuildMercury(t, repoRoot)
	chBin, err := buildCh(repoRoot)
	if err != nil {
		t.Skipf("channel simulator unavailable: %v", err)
	}

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
			"-m", "1", "-v", // DATAC3
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

	bridge := startChannelBridge(ctx, chBin, aTX, bRX, bTX, aRX, channelParamsFor(t))
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
	ctlB, err := waitForTCP(ctx, "127.0.0.1", bPort, controlPortTimeout, procB)
	if err != nil {
		fail("B control port: %v", err)
	}
	defer ctlB.Close()
	rwA := bufio.NewReadWriter(bufio.NewReader(ctlA), bufio.NewWriter(ctlA))
	rwB := bufio.NewReadWriter(bufio.NewReader(ctlB), bufio.NewWriter(ctlB))
	// B listens for calls, so its MFSK call listener runs.
	if got := sendControlCommand(t, ctlB, rwB, "MYCALL TESTB"); !strings.HasPrefix(got, "OK") {
		fail("MYCALL -> %q", got)
	}
	if line, err := rwB.ReadString('\r'); err != nil || !strings.HasPrefix(line, "REGISTERED") {
		fail("after MYCALL: %q %v", line, err)
	}
	if got := sendControlCommand(t, ctlB, rwB, "LISTEN ON"); !strings.HasPrefix(got, "OK") {
		fail("LISTEN ON -> %q", got)
	}

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
	got := make(chan []byte, 16)
	go kissRead(bcastB, got, 0)

	for _, c := range []struct {
		index, len, frameSize int
		layout                byte
	}{
		{8, 14, 14, 0},  // DATAC16, the compact layout
		{11, 26, 98, 1}, // MFSK, the full layout
		{12, 26, 98, 1}, // MFSK500: B's other call listener
	} {
		// The last beacon's PTT, BUFFER and BITRATE lines may still be ahead of
		// the reply.
		r := sendControlCommand(t, ctlA, rwA, fmt.Sprintf("BEACONMODE %d", c.index))
		for n := 0; r != "OK" && r != "WRONG" && n < 16; n++ {
			line, err := rwA.ReadString('\r')
			if err != nil {
				fail("BEACONMODE %d: %v", c.index, err)
			}
			r = strings.TrimSuffix(line, "\r")
		}
		if !strings.HasPrefix(r, "OK") {
			fail("BEACONMODE %d -> %q", c.index, r)
		}
		beacon := make([]byte, c.len)
		beacon[0] = 0x07<<5 | c.layout
		for i := 1; i < c.len; i++ {
			beacon[i] = byte(c.index*7 + i)
		}
		if _, err := bcastA.Write(kissEncode(kissCmdModemFrame, beacon)); err != nil {
			fail("write beacon: %v", err)
		}
		deadline := time.After(90 * time.Second)
	wait:
		for {
			select {
			case f, ok := <-got:
				if !ok {
					fail("B broadcast port closed")
				}
				if len(f) < 2 || f[1] != beacon[0] {
					continue
				}
				if f[0] != kissCmdModemFrame || len(f)-1 != c.frameSize ||
					string(f[1:1+c.len]) != string(beacon) {
					fail("mode %d: B got cmd 0x%02x, %d bytes %x", c.index, f[0], len(f)-1, f[1:])
				}
				t.Logf("B heard the beacon on mode index %d (%d bytes)", c.index, len(f)-1)
				break wait
			case <-deadline:
				fail("mode %d: B never heard the beacon", c.index)
			}
		}
	}
}
