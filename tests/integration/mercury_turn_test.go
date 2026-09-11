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

// TestMercuryARQTurnHandoff exercises the TURN_REQ/TURN_ACK role change with
// real binaries and real modems -- the path that TestMercuryARQBidirectional
// never reaches.
//
// That test queues both payloads AT ONCE, so both stations always have data and
// the floor is handed over by the piggyback HAS_DATA flag on every ACK.  A
// TURN_REQ is only ever sent by an IRS whose peer has gone IDLE, so the whole
// TURN_REQ/TURN_ACK exchange is dead code as far as the integration suite is
// concerned: measured zero TURN_REQ frames across a full bidirectional run.
//
// That blind spot is why three separate dflow states were found ignoring events
// they should handle -- WAIT_ACK and TURN_REQ_WAIT ignoring RX_TURN_REQ, and
// KEEPALIVE_WAIT discarding RX_DATA -- with CI green throughout.
//
// So sequence it instead, which is also the real-world shape (a store-and-
// forward batch completes, then the other end has traffic):
//
//  1. A connects and sends its payload; wait until B has ALL of it.
//  2. Let A fall idle, so it holds the floor with nothing to send.
//  3. THEN B's application queues data.  B is the IRS with an idle peer, so it
//     must request the turn, and A must grant it.
//  4. B's payload must arrive at A.
//
// Step 3 is the assertion that matters: the test verifies from B's own log that
// a TURN_REQ was actually sent.  Without that check a passing run proves
// nothing, because the piggyback path could have carried the data instead.
//
// MERCURY_CH_NO adds noise; loss is what makes a TURN_ACK go missing and puts
// both ends into the contested state.
func TestMercuryARQTurnHandoff(t *testing.T) {
	repoRoot := mustRepoRoot(t)
	bin := locateOrBuildMercury(t, repoRoot)

	params := DefaultChannelParams()
	// Channel engine: clean ch.c (default) or the Watterson HF fading model.
	// The on-air DATAC15 data-decode failure does NOT reproduce on the clean
	// channel, so set MERCURY_CH_ENGINE=watterson (+ MERCURY_CH_FADING) to add
	// realistic slow/deep fades and a finite SNR closer to the real link.
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
		no, perr := strconv.ParseFloat(v, 64)
		if perr != nil {
			t.Fatalf("bad MERCURY_CH_NO %q: %v", v, perr)
		}
		params.No_dBHz = no
	}
	if v := os.Getenv("MERCURY_CH_FADING"); v != "" {
		params.Fading = v // ch: mpg|mpp|mpd ; watterson: good|moderate|poor
	}
	if v := os.Getenv("MERCURY_CH_GAIN"); v != "" {
		g, perr := strconv.ParseFloat(v, 64)
		if perr != nil {
			t.Fatalf("bad MERCURY_CH_GAIN %q: %v", v, perr)
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
			"-v",
			"-C", filepath.Join(t.TempDir(), "missing-mercury.ini"),
		)
		cmd.Dir = repoRoot
		cmd.Stdout = stdout
		cmd.Stderr = stderr
		if err := startChild(cmd); err != nil {
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
		conn       net.Conn
		rw         *bufio.ReadWriter
		cmd        string
		drainAfter string
	}{
		{connA, rwA, "MYCALL TESTA", "REGISTERED TESTA"},
		{connB, rwB, "MYCALL TESTB", "REGISTERED TESTB"},
		{connB, rwB, "LISTEN ON", ""},
	} {
		if got := sendControlCommand(t, c.conn, c.rw, c.cmd); !strings.HasPrefix(got, "OK") {
			failWithLogs("%q -> %q, want OK", c.cmd, got)
		}
		if c.drainAfter != "" {
			line, err := c.rw.ReadString('\r')
			if err != nil {
				failWithLogs("drain after %q: %v", c.cmd, err)
			}
			line = strings.TrimSuffix(line, "\r")
			if !strings.HasPrefix(line, c.drainAfter) {
				failWithLogs("unsolicited after %q: got %q, want prefix %q", c.cmd, line, c.drainAfter)
			}
		}
	}

	// Open both data ports before connecting so delivery is observable in
	// both directions.
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

	// --- Step 1: A sends, and B must receive all of it. ---------------------
	payloadA := []byte(strings.Repeat("MERCURY-TURN-A2B-0123456789", 4)) // ~108 B
	payloadB := []byte(strings.Repeat("MERCURY-TURN-B2A-0123456789", 4)) // ~108 B

	recvAll := func(name string, conn net.Conn, want []byte, within time.Duration) (int, bool) {
		start := time.Now()
		got := make([]byte, 0, len(want))
		buf := make([]byte, 4096)
		deadline := time.Now().Add(within)
		for len(got) < len(want) && time.Now().Before(deadline) {
			_ = conn.SetReadDeadline(time.Now().Add(10 * time.Second))
			n, err := conn.Read(buf)
			if n > 0 {
				got = append(got, buf[:n]...)
			}
			if err != nil {
				if ne, ok := err.(net.Error); ok && ne.Timeout() {
					continue
				}
				break
			}
		}
		t.Logf("%s delivered %d/%d bytes in %s", name, len(got), len(want),
			time.Since(start).Round(time.Millisecond))
		return len(got), string(got) == string(want)
	}

	if _, err := dataA.Write(payloadA); err != nil {
		failWithLogs("A write payload: %v", err)
	}
	if n, ok := recvAll("B<-A", dataB, payloadA, 4*time.Minute); !ok {
		failWithLogs("A->B incomplete (%d/%d) before the handoff could be tested",
			n, len(payloadA))
	}

	// --- Step 2: let A fall idle, holding the floor with nothing to send. ---
	// Long enough for A's last ACK to land and its data flow to settle; short
	// enough to stay well inside the keepalive interval, so what follows is a
	// turn request and not keepalive recovery.
	time.Sleep(12 * time.Second)

	// --- Step 3+4: B now has data.  It must ASK for the floor. --------------
	if _, err := dataB.Write(payloadB); err != nil {
		failWithLogs("B write payload: %v", err)
	}
	n, ok := recvAll("A<-B", dataA, payloadB, 4*time.Minute)
	if !ok {
		failWithLogs("B->A incomplete (%d/%d): the IRS could not take the floor "+
			"from an idle peer", n, len(payloadB))
	}

	// The assertion that makes this a test of the role change.  If the payload
	// arrived without a TURN_REQ, the piggyback path carried it and the
	// TURN_REQ/TURN_ACK exchange is still untested.
	logB, rerr := os.ReadFile(errB.Name())
	if rerr != nil {
		t.Fatalf("read B log: %v", rerr)
	}
	if !strings.Contains(string(logB), "TURN_REQ") {
		failWithLogs("B delivered its payload without ever sending a TURN_REQ — " +
			"the turn was taken by piggyback, so the role-change path is STILL " +
			"untested; the idle-peer sequencing this test depends on has broken")
	}
	turnReqs := strings.Count(string(logB), "TURN_REQ")
	t.Logf("turn handoff exercised: B sent/handled %d TURN_REQ log events", turnReqs)

	t.Logf("turn-handoff ARQ exchange complete over ch (No=%.1f dB): "+
		"idle-peer floor request granted, both directions delivered", params.No_dBHz)
}
