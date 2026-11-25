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

// A caller connected on an ACCEPT whose callee had already let the call go
// gives up when it has heard nothing of the callee's in 120 s, not at the
// 240 s lost-peer watchdog.  On air (KG9NI -> K7EK, 2026-11-24 04:23Z)
// BPQ32's LISTEN OFF came 1.9 s into K7EK's ACCEPT, K7EK cancelled, the
// ACCEPT played on, and KG9NI, "Connected to K7EK", polled nobody for 258 s.
// A cut ACCEPT does not help: a 14-byte DATAC16 ACCEPT decodes from its
// first 1.3 s, and the LISTEN OFF grace is 2 s.
//
// A calls B; B's host sends LISTEN OFF 1.5 s into B's ACCEPT, and A
// connects all the same.  A's host must hear DISCONNECTED within 180 s of
// CONNECTED: 120 s, an answer due to a poll on the air at the mark, then
// its one DISCONNECT (about 253 s on mercuryv2: 240 s, then the same).
func TestSessionNeverStartedEndsEarly(t *testing.T) {
	repoRoot := mustRepoRoot(t)
	bin := locateOrBuildMercury(t, repoRoot)

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

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	start := func(name, rx, tx string, port int) (*exec.Cmd, *processWait, *os.File, *os.File) {
		stdout, stderr := tempLogFilesNamed(t, name)
		cmd := exec.CommandContext(ctx, bin, "-x", "fifo", "-i", rx, "-o", tx,
			"-p", fmt.Sprint(port), "-b", fmt.Sprint(port+100), "-m", "1",
			"-C", filepath.Join(t.TempDir(), "missing-mercury.ini"))
		cmd.Dir = repoRoot
		cmd.Stdout = stdout
		cmd.Stderr = stderr
		if err := startChild(cmd); err != nil {
			t.Fatalf("start mercury %s: %v", name, err)
		}
		return cmd, waitForProcess(cmd), stdout, stderr
	}
	aPort, bPort := freePortPair(t), freePortPair(t)
	cmdA, procA, outA, errA := start("A", aRX, aTX, aPort)
	defer func() { _ = stopProcess(t, cmdA, procA, outA.Name(), errA.Name()) }()
	cmdB, procB, outB, errB := start("B", bRX, bTX, bPort)
	defer func() { _ = stopProcess(t, cmdB, procB, outB.Name(), errB.Name()) }()
	logs := func() {
		printLogs(t, outA.Name(), errA.Name())
		printLogs(t, outB.Name(), errB.Name())
	}

	// Paced, as on the air: an unpaced bridge has the ACCEPT arrive while
	// the caller is still keyed on its CALL, deaf to it, and the caller
	// never connects whether the ACCEPT is cut or not.
	chBin, err := buildCh(repoRoot)
	if err != nil {
		t.Skipf("channel simulator: %v", err)
	}
	bridge := startChannelBridge(ctx, chBin, aTX, bRX, bTX, aRX, channelParamsFor(t))
	defer bridge.Close()

	connA, err := waitForTCP(ctx, "127.0.0.1", aPort, controlPortTimeout, procA)
	if err != nil {
		logs()
		t.Fatalf("mercury A control port: %v", err)
	}
	defer connA.Close()
	connB, err := waitForTCP(ctx, "127.0.0.1", bPort, controlPortTimeout, procB)
	if err != nil {
		logs()
		t.Fatalf("mercury B control port: %v", err)
	}
	defer connB.Close()
	rwA := bufio.NewReadWriter(bufio.NewReader(connA), bufio.NewWriter(connA))
	rwB := bufio.NewReadWriter(bufio.NewReader(connB), bufio.NewWriter(connB))

	setup := func(conn net.Conn, rw *bufio.ReadWriter, call string, cmds ...string) {
		for _, c := range append([]string{"MYCALL " + call}, cmds...) {
			if got := sendControlCommand(t, conn, rw, c); !strings.HasPrefix(got, "OK") {
				logs()
				t.Fatalf("%s %q: got %q", call, c, got)
			}
			if strings.HasPrefix(c, "MYCALL") {
				if line, err := rw.ReadString('\r'); err != nil || !strings.HasPrefix(line, "REGISTERED") {
					t.Fatalf("%s after MYCALL: %q, %v", call, line, err)
				}
			}
		}
	}
	setup(connA, rwA, "TESTA")
	setup(connB, rwB, "TESTB", "LISTEN ON")

	send := func(conn net.Conn, rw *bufio.ReadWriter, line string) {
		if err := conn.SetWriteDeadline(time.Now().Add(commandTimeout)); err != nil {
			t.Fatal(err)
		}
		if _, err := rw.WriteString(line + "\r"); err != nil {
			t.Fatal(err)
		}
		if err := rw.Flush(); err != nil {
			t.Fatal(err)
		}
	}

	// A's control lines, in the background: CONNECTED must never come.
	aLines := make(chan string, 256)
	go func() {
		for {
			_ = connA.SetReadDeadline(time.Now().Add(3 * time.Minute))
			line, err := rwA.ReadString('\r')
			if err != nil {
				close(aLines)
				return
			}
			aLines <- strings.TrimSuffix(line, "\r")
		}
	}()
	send(connA, rwA, "CONNECT TESTA TESTB")

	// B: PENDING, then its ACCEPT keys (PTT ON); 1.5 s into it, LISTEN OFF.
	var bLines []string
	pending := false
	deadline := time.Now().Add(90 * time.Second)
	for {
		if time.Now().After(deadline) {
			logs()
			t.Fatalf("B never keyed an ACCEPT:\n%s", strings.Join(bLines, "\n"))
		}
		_ = connB.SetReadDeadline(deadline)
		line, err := rwB.ReadString('\r')
		if err != nil {
			logs()
			t.Fatalf("B's control port: %v\n%s", err, strings.Join(bLines, "\n"))
		}
		line = strings.TrimSuffix(line, "\r")
		bLines = append(bLines, line)
		if line == "PENDING" {
			pending = true
		}
		if strings.HasPrefix(line, "CONNECTED") {
			logs()
			t.Fatalf("B connected before its host let the call go:\n%s", strings.Join(bLines, "\n"))
		}
		if pending && line == "PTT ON" {
			break
		}
	}
	time.Sleep(1500 * time.Millisecond)
	send(connB, rwB, "LISTEN OFF")
	tOff := time.Now()

	// A connects on the ACCEPT, hears nothing more, and gives up.
	var aSeen []string
	var connectedAt time.Time
	limit := time.After(5 * time.Minute)
	for ended := false; !ended; {
		select {
		case line, ok := <-aLines:
			if !ok {
				logs()
				t.Fatalf("A's control port closed:\n%s", strings.Join(aSeen, "\n"))
			}
			aSeen = append(aSeen, line)
			switch {
			case strings.HasPrefix(line, "CONNECTED"):
				connectedAt = time.Now()
			case strings.HasPrefix(line, "DISCONNECTED") && !connectedAt.IsZero():
				ended = true
			}
		case <-limit:
			logs()
			t.Fatalf("A still in the session 5 min on:\n%s", strings.Join(aSeen, "\n"))
		}
	}
	if connectedAt.IsZero() {
		t.Skipf("A never connected (the ACCEPT was not decoded): nothing to test\n%s", strings.Join(aSeen, "\n"))
	}
	held := time.Since(connectedAt)
	time.Sleep(time.Second) // the async logger
	b, _ := os.ReadFile(errA.Name())
	if !strings.Contains(string(b), "the session never started") {
		logs()
		t.Fatalf("A's log never said the session never started (ended %.1f s after CONNECTED)", held.Seconds())
	}
	if held > 180*time.Second {
		logs()
		t.Fatalf("A held the session %.1f s after CONNECTED, want at most 180 s", held.Seconds())
	}
	t.Logf("A connected %.1f s after B's LISTEN OFF and gave up %.1f s later", connectedAt.Sub(tOff).Seconds(), held.Seconds())
}
