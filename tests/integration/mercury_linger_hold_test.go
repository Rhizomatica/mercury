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
	"sync/atomic"
	"syscall"
	"testing"
	"time"
)

// After a session whose DISCONNECT the peer never answered, Mercury keeps
// the session's seed for the peer's polls (the linger, arq_fsm.c
// end_unanswered) and holds a scanning host on the channel meanwhile
// (arq.c, linger_hold).  Without the hold the linger listened on a channel
// the host had left: on air (K7EK, 2026-11-27 02:44:57Z) BPQ32 hopped 2.5 s
// after the ABORTed session ended, and KG9NI, which had missed the
// DISCONNECT, polled for 4 minutes.
//
// A calls B and they connect.  Then B's audio stops reaching A, and B's host
// ABORTs: B's one DISCONNECT goes nowhere, and A polls on.  After the
// ABORT's own hold, B's host must be held on PENDING (no gap over 25 s)
// until the linger ends with one CANCELPENDING, its two answers to A's polls
// spent or its time up.
func TestLingerHoldsTheHostForAPeerThatMissedOurDisconnect(t *testing.T) {
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

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
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

	var bMuted atomic.Bool
	go pacedBridge(ctx, aTX, bRX, func() bool { return true })
	go pacedBridge(ctx, bTX, aRX, func() bool { return !bMuted.Load() })

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
	_ = connA.SetDeadline(time.Time{})
	_ = connB.SetDeadline(time.Time{})

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
	// A's control port is read so that it never backs up.
	go func() {
		for {
			if _, err := rwA.ReadString('\r'); err != nil {
				return
			}
		}
	}()
	send(connA, rwA, "CONNECT TESTA TESTB")

	// B's control lines: CONNECTED, then (B muted) ABORT, then the holds.
	const watch = 200 * time.Second
	const maxGap = 25 * time.Second
	begin := time.Now()
	var lines []string
	var aborted, lingerFrom, released time.Duration
	var lingerPendings []time.Duration
	abortHeld := false
	for time.Since(begin) < watch && released == 0 {
		if err := connB.SetReadDeadline(time.Now().Add(watch - time.Since(begin))); err != nil {
			t.Fatal(err)
		}
		line, err := rwB.ReadString('\r')
		if err != nil {
			break
		}
		line = strings.TrimSuffix(line, "\r")
		at := time.Since(begin)
		lines = append(lines, fmt.Sprintf("%5.1f %s", at.Seconds(), line))
		switch {
		case strings.HasPrefix(line, "CONNECTED") && aborted == 0:
			// Let the session settle on its first exchanges, then cut B off.
			time.Sleep(8 * time.Second)
			bMuted.Store(true)
			send(connB, rwB, "ABORT")
			aborted = time.Since(begin)
		case aborted == 0:
		case line == "PENDING" && lingerFrom == 0 && !abortHeld:
			abortHeld = true // the ABORT's hold, until our DISCONNECT is out
		case line == "CANCELPENDING" && abortHeld && lingerFrom == 0:
			lingerFrom = -1 // the ABORT's hold is over: the linger's comes next
		case line == "PENDING" && lingerFrom != 0:
			if lingerFrom < 0 {
				lingerFrom = at
			}
			lingerPendings = append(lingerPendings, at)
		case line == "CANCELPENDING" && lingerFrom > 0:
			released = at
		}
	}
	if len(lingerPendings) == 0 || released == 0 {
		logs()
		t.Fatalf("want the ABORT's hold, then PENDING ... CANCELPENDING for the linger, got:\n%s",
			strings.Join(lines, "\n"))
	}
	worst := released - lingerPendings[len(lingerPendings)-1]
	for i := 1; i < len(lingerPendings); i++ {
		if g := lingerPendings[i] - lingerPendings[i-1]; g > worst {
			worst = g
		}
	}
	if worst > maxGap {
		logs()
		t.Fatalf("the linger held the host %.1f s, longest gap %.1f s (want <= %v):\n%s",
			(released - lingerFrom).Seconds(), worst.Seconds(), maxGap, strings.Join(lines, "\n"))
	}
	if held := released - lingerFrom; held < 10*time.Second || held > 70*time.Second {
		logs()
		t.Fatalf("the linger held the host %.1f s (want 10-70 s: until A's polls are answered or its 60 s are up):\n%s",
			held.Seconds(), strings.Join(lines, "\n"))
	}
	b, _ := os.ReadFile(errB.Name()) // the log goes to stderr
	if !strings.Contains(string(b), "still polling the session we ended") {
		logs()
		t.Fatalf("B never answered a poll of A's during the linger:\n%s", strings.Join(lines, "\n"))
	}
	t.Logf("linger held the host %.1f s with %d PENDING, longest gap %.1f s",
		(released - lingerFrom).Seconds(), len(lingerPendings), worst.Seconds())
}
