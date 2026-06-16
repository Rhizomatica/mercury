package integration

import (
	"bufio"
	"context"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

// TestMercuryARQTransfer drives a full ARQ session between two Mercury
// instances bridged through the ch channel simulator: CONNECT handshake
// (DATAC16 control frames), payload transfer (DATAC15 data frames) and
// delivery verification on the peer data port.
//
// MERCURY_CH_NO can be set to a noise density in dB (e.g. "-8" for an SNR
// of roughly -6 dB in 3 kHz) to exercise the link under channel noise;
// the default -100 is effectively a clean channel.
func TestMercuryARQTransfer(t *testing.T) {
	repoRoot := mustRepoRoot(t)
	bin := locateOrBuildMercury(t, repoRoot)

	params := DefaultChannelParams()

	// Channel engine: codec2 ch.c (default) or the Watterson HF model.  The
	// Watterson model gives controllable slow/deep fades (ITU-R good/moderate/
	// poor) that reach the gear-shift oscillation regime ch --mpp cannot.
	var chBin string
	var err error
	if os.Getenv("MERCURY_CH_ENGINE") == "watterson" {
		params.Engine = "watterson"
		chBin, err = buildWatterson(repoRoot)
	} else {
		chBin, err = buildCh(repoRoot)
	}
	if err != nil {
		t.Skipf("channel simulator unavailable: %v", err)
	}

	if v := os.Getenv("MERCURY_CH_NO"); v != "" {
		no, err := strconv.ParseFloat(v, 64)
		if err != nil {
			t.Fatalf("bad MERCURY_CH_NO %q: %v", v, err)
		}
		params.No_dBHz = no
	}
	if v := os.Getenv("MERCURY_CH_FADING"); v != "" {
		params.Fading = v // mpg | mpp | mpd
	}
	if v := os.Getenv("MERCURY_CH_GAIN"); v != "" {
		g, err := strconv.ParseFloat(v, 64)
		if err != nil {
			t.Fatalf("bad MERCURY_CH_GAIN %q: %v", v, err)
		}
		params.Gain = g
	}

	dir := t.TempDir()
	aRX := filepath.Join(dir, "a_rx.s32le.fifo")
	aTX := filepath.Join(dir, "a_tx.s32le.fifo")
	bRX := filepath.Join(dir, "b_rx.s32le.fifo")
	bTX := filepath.Join(dir, "b_tx.s32le.fifo")
	for _, p := range []string{aRX, aTX, bRX, bTX} {
		if err := syscall.Mkfifo(p, 0600); err != nil {
			t.Fatalf("mkfifo %s: %v", p, err)
		}
	}

	aPort := freePortPair(t)
	bPort := freePortPair(t)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	startInstance := func(name, rxPath, txPath string, port, bcastPort int) (*exec.Cmd, *processWait, *os.File, *os.File) {
		stdout, stderr := tempLogFilesNamed(t, name)
		cmd := exec.CommandContext(ctx, bin,
			"-x", "fifo",
			"-i", rxPath,
			"-o", txPath,
			"-p", fmt.Sprint(port),
			"-b", fmt.Sprint(bcastPort),
			"-m", "1",
			"-C", filepath.Join(t.TempDir(), "missing-mercury.ini"),
		)
		cmd.Dir = repoRoot
		cmd.Stdout = stdout
		cmd.Stderr = stderr
		if err := cmd.Start(); err != nil {
			t.Fatalf("start mercury %s: %v", name, err)
		}
		return cmd, waitForProcess(cmd), stdout, stderr
	}

	cmdA, procA, outA, errA := startInstance("A", aRX, aTX, aPort, aPort+100)
	defer func() { _ = stopProcess(t, cmdA, procA, outA.Name(), errA.Name()) }()
	cmdB, procB, outB, errB := startInstance("B", bRX, bTX, bPort, bPort+100)
	defer func() { _ = stopProcess(t, cmdB, procB, outB.Name(), errB.Name()) }()

	bridge := startChannelBridge(ctx, chBin, aTX, bRX, bTX, aRX, params)
	defer bridge.Close()

	failWithLogs := func(format string, args ...interface{}) {
		printLogs(t, outA.Name(), errA.Name())
		printLogs(t, outB.Name(), errB.Name())
		t.Fatalf(format, args...)
	}

	connA, err := waitForTCP(ctx, "127.0.0.1", aPort, controlPortTimeout, procA)
	if err != nil {
		failWithLogs("mercury A control port: %v", err)
	}
	defer connA.Close()
	connB, err := waitForTCP(ctx, "127.0.0.1", bPort, controlPortTimeout, procB)
	if err != nil {
		failWithLogs("mercury B control port: %v", err)
	}
	defer connB.Close()

	rwA := bufio.NewReadWriter(bufio.NewReader(connA), bufio.NewWriter(connA))
	rwB := bufio.NewReadWriter(bufio.NewReader(connB), bufio.NewWriter(connB))

	for _, c := range []struct {
		conn net.Conn
		rw   *bufio.ReadWriter
		cmd  string
	}{
		{connA, rwA, "MYCALL TESTA"},
		{connB, rwB, "MYCALL TESTB"},
		{connB, rwB, "LISTEN ON"},
	} {
		if got := sendControlCommand(t, c.conn, c.rw, c.cmd); !strings.HasPrefix(got, "OK") {
			failWithLogs("%q -> %q, want OK", c.cmd, got)
		}
	}

	// Open data ports before connecting so payload delivery is observable.
	dataA, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", aPort+1), 5*time.Second)
	if err != nil {
		failWithLogs("mercury A data port: %v", err)
	}
	defer dataA.Close()
	dataB, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", bPort+1), 5*time.Second)
	if err != nil {
		failWithLogs("mercury B data port: %v", err)
	}
	defer dataB.Close()

	if got := sendControlCommand(t, connA, rwA, "CONNECT TESTA TESTB"); !strings.HasPrefix(got, "OK") {
		failWithLogs("CONNECT -> %q, want OK", got)
	}

	// Wait for the CONNECTED notification on A's control port.  Control
	// notifications are CR-terminated; DISCONNECTED must not match.
	connectedDeadline := time.Now().Add(3 * time.Minute)
	if err := connA.SetReadDeadline(connectedDeadline); err != nil {
		t.Fatal(err)
	}
	connected := false
	for time.Now().Before(connectedDeadline) && !connected {
		line, err := rwA.ReadString('\r')
		if err != nil {
			break
		}
		line = strings.TrimSpace(line)
		if line != "" {
			t.Logf("A control: %s", line)
		}
		if strings.HasPrefix(line, "CONNECTED") {
			connected = true
		}
		if strings.HasPrefix(line, "DISCONNECTED") {
			failWithLogs("link disconnected before CONNECT completed")
		}
	}
	if !connected {
		failWithLogs("no CONNECTED notification within deadline")
	}
	_ = connA.SetReadDeadline(time.Time{})

	// Default 102 bytes (3 datac15 frames).  MERCURY_TEST_PAYLOAD_KB sets a
	// larger transfer to exercise the speed-ladder climb and the >511-byte
	// DATA valid-length encoding (datac17/qam16c2 frames).
	repeats := 3
	if v := os.Getenv("MERCURY_TEST_PAYLOAD_KB"); v != "" {
		kb, err := strconv.Atoi(v)
		if err != nil || kb <= 0 {
			t.Fatalf("bad MERCURY_TEST_PAYLOAD_KB %q", v)
		}
		repeats = (kb*1024 + 33) / 34
	}
	payload := []byte(strings.Repeat("MERCURY-DATAC15-PAYLOAD-0123456789", repeats))
	if _, err := dataA.Write(payload); err != nil {
		failWithLogs("write payload: %v", err)
	}

	rx := make([]byte, 0, len(payload))
	buf := make([]byte, 4096)
	rxDeadline := time.Now().Add(5*time.Minute +
		time.Duration(len(payload)/100)*time.Second)
	for len(rx) < len(payload) && time.Now().Before(rxDeadline) {
		if err := dataB.SetReadDeadline(time.Now().Add(15 * time.Second)); err != nil {
			t.Fatal(err)
		}
		n, err := dataB.Read(buf)
		if n > 0 {
			rx = append(rx, buf[:n]...)
			t.Logf("B data: %d/%d bytes", len(rx), len(payload))
		}
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				continue
			}
			break
		}
	}
	if string(rx) != string(payload) {
		failWithLogs("payload mismatch: got %d bytes, want %d", len(rx), len(payload))
	}

	// The transfer proves the modes on the air; the logs document them.
	for name, paths := range map[string][2]string{
		"A": {outA.Name(), errA.Name()},
		"B": {outB.Name(), errB.Name()},
	} {
		var log strings.Builder
		for _, path := range paths {
			data, err := os.ReadFile(path)
			if err != nil {
				t.Fatalf("read %s log: %v", name, err)
			}
			log.Write(data)
		}
		for _, mode := range []string{"DATAC16", "DATAC15"} {
			if !strings.Contains(log.String(), mode) {
				failWithLogs("instance %s log has no %s activity", name, mode)
			}
		}
	}
	t.Logf("ARQ transfer complete over ch (No=%.1f dB): %d bytes delivered", params.No_dBHz, len(payload))
}
