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

// TestMercuryARQBidirectional exercises the half-duplex turn-taking that the
// one-way TestMercuryARQTransfer structurally cannot reach.  Both endpoints
// queue application data at the same time, so the receiver (IRS) must gain the
// floor to deliver its own payload while the initiator (ISS) is mid-transfer.
//
// That is the WAIT_ACK + RX_TURN_REQ contention that deadlocked the link before
// commit 11a6a9f: the ISS, awaiting the ACK of its last burst, ignored the
// peer's TURN_REQ, sat out the ack-timeout and retransmitted while the peer
// kept re-requesting the floor — a mutual stall.  Pre-fix this test times out
// (neither payload completes); post-fix both directions deliver.
//
// The channel is clean (default No=-100) so a failure is unambiguously the turn
// logic, not the link.  MERCURY_CH_NO may be set to add noise.
func TestMercuryARQBidirectional(t *testing.T) {
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

	// Both sides queue data at once.  A holds the floor (it issued CONNECT);
	// B's queued payload forces it to request the turn while A is still
	// sending — the contention the deadlock fix addresses.  Kept small (a few
	// datac15 frames each way) so the test turns the floor over several times
	// without depending on throughput.
	payloadA := []byte(strings.Repeat("MERCURY-BIDIR-A2B-0123456789", 4)) // ~112 B, A -> B
	payloadB := []byte(strings.Repeat("MERCURY-BIDIR-B2A-0123456789", 4)) // ~112 B, B -> A

	if _, err := dataA.Write(payloadA); err != nil {
		failWithLogs("A write payload: %v", err)
	}
	if _, err := dataB.Write(payloadB); err != nil {
		failWithLogs("B write payload: %v", err)
	}

	type result struct {
		name string
		got  int
		ok   bool
		dur  time.Duration
	}
	results := make(chan result, 2)
	recv := func(name string, conn net.Conn, want []byte) {
		start := time.Now()
		got := make([]byte, 0, len(want))
		buf := make([]byte, 4096)
		deadline := time.Now().Add(4 * time.Minute)
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
		results <- result{name, len(got), string(got) == string(want), time.Since(start)}
	}
	go recv("B<-A", dataB, payloadA)
	go recv("A<-B", dataA, payloadB)

	for i := 0; i < 2; i++ {
		r := <-results
		want := len(payloadA)
		if r.name == "A<-B" {
			want = len(payloadB)
		}
		t.Logf("%s delivered %d/%d bytes in %s", r.name, r.got, want, r.dur.Round(time.Millisecond))
		if !r.ok {
			failWithLogs("%s: incomplete delivery %d/%d bytes — turnaround likely deadlocked",
				r.name, r.got, want)
		}
	}
	t.Logf("bidirectional ARQ exchange complete over ch (No=%.1f dB): both directions delivered, turn-taking healthy",
		params.No_dBHz)
}
