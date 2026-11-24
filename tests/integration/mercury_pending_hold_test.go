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
	"sync"
	"syscall"
	"testing"
	"time"
)

// While answering a call, Mercury tells the host PENDING at least every 20 s
// until it stops answering (arq.c, pending_hold).  BPQ32 holds its scanner
// for six scan intervals per PENDING (VARA.c), about a minute on air, while
// Mercury answers for as long as its ACCEPT retries last (RETRIES slots from
// the last CALL heard).  Only a repeat of the CALL that was decoded sent
// PENDING again, and on a weak path the repeats go undecoded: on air (K7EK,
// 2026-11-12 20:49) BPQ32 hopped 66 s after the one PENDING while the caller
// called on for 2 minutes.
//
// A calls B once and goes silent (its host's DISCONNECT, and it never hears
// B's ACCEPTs: B's audio goes nowhere).  B answers until its 8 ACCEPT retries
// run out, then says CANCELPENDING.
func TestAnsweringHoldsTheHostChannel(t *testing.T) {
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

	// A's audio reaches B; B's goes nowhere, and A hears silence.
	bridgeCtx, bridgeCancel := context.WithCancel(ctx)
	defer bridgeCancel()
	go bridgeDirection(bridgeCtx, aTX, bRX, "A_TX->B_RX")
	go drainFIFO(bridgeCtx, bTX)
	go silenceFIFO(bridgeCtx, aRX)

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
	setup(connB, rwB, "TESTB", "RETRIES 8", "LISTEN ON")

	send := func(rw *bufio.ReadWriter, line string) {
		if _, err := rw.WriteString(line + "\r"); err != nil {
			t.Fatal(err)
		}
		if err := rw.Flush(); err != nil {
			t.Fatal(err)
		}
	}
	send(rwA, "CONNECT TESTA TESTB")

	// B's control lines from here on; A hangs up at B's first PENDING.
	const watch = 150 * time.Second
	const maxGap = 25 * time.Second
	begin := time.Now()
	var pendings []time.Duration
	var cancelled time.Duration
	var lines []string
	for time.Since(begin) < watch && cancelled == 0 {
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
		case line == "PENDING":
			if len(pendings) == 0 {
				send(rwA, "DISCONNECT")
			}
			pendings = append(pendings, at)
		case line == "CANCELPENDING":
			cancelled = at
		case strings.HasPrefix(line, "CONNECTED"):
			logs()
			t.Fatalf("B connected, A should never hear its ACCEPT:\n%s", strings.Join(lines, "\n"))
		}
	}
	if len(pendings) == 0 || cancelled == 0 {
		logs()
		t.Fatalf("want PENDING ... CANCELPENDING from B, got:\n%s", strings.Join(lines, "\n"))
	}
	worst := cancelled - pendings[len(pendings)-1]
	for i := 1; i < len(pendings); i++ {
		if g := pendings[i] - pendings[i-1]; g > worst {
			worst = g
		}
	}
	if worst > maxGap {
		logs()
		t.Fatalf("%d PENDING over %.1f s of answering, longest gap %.1f s (want <= %v):\n%s",
			len(pendings), (cancelled - pendings[0]).Seconds(), worst.Seconds(), maxGap,
			strings.Join(lines, "\n"))
	}
	t.Logf("%d PENDING over %.1f s of answering, longest gap %.1f s",
		len(pendings), (cancelled - pendings[0]).Seconds(), worst.Seconds())
}

// The ACCEPT fallback holds the host on PENDING for 90 s after the callee's
// last ACCEPT (ARQ_ACCEPT_FALLBACK_HOLD_MS, #657): a caller that took that
// ACCEPT may be connected and polling although none of its frames decoded.
// arrival_hold must stand aside meanwhile: a frame that locks a decoder and
// is no CALL for us -- a poll that does not decode, or here another station's
// CALL -- ended in CANCELPENDING 2 s after the lock, and a scanning host
// hopped in the middle of the hold (#657 review, 77bcbd4c).
//
// A calls B once and hangs up once B answers it; B answers (default 4
// ACCEPT slots, so its retries end well inside the hold).  55 s after B's
// ACCEPT, A calls a station that is not B, and its CALLs reach B.  B's host
// must hear no CANCELPENDING before the hold's end, then exactly the one.
func TestAcceptFallbackHoldOutlastsAForeignLock(t *testing.T) {
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

	// A's audio reaches B, paced in real time as a radio's is: a decoder's
	// lock then lasts as long as the frame, and arrival_hold sees it.  B's
	// audio goes nowhere, and A hears silence.
	bridgeCtx, bridgeCancel := context.WithCancel(ctx)
	defer bridgeCancel()
	go pacedBridge(bridgeCtx, aTX, bRX, func() bool { return true })
	go drainFIFO(bridgeCtx, bTX)
	go silenceFIFO(bridgeCtx, aRX)

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
	// sendControlCommand leaves its deadline on the connection: A is written
	// to a minute and more from now.
	_ = connA.SetDeadline(time.Time{})
	_ = connB.SetDeadline(time.Time{})

	send := func(rw *bufio.ReadWriter, line string) {
		if _, err := rw.WriteString(line + "\r"); err != nil {
			t.Fatal(err)
		}
		if err := rw.Flush(); err != nil {
			t.Fatal(err)
		}
	}

	begin := time.Now()
	// A's control lines: when its transmitter keys (its CALLs to OTHER).
	var aMu sync.Mutex
	var aKeyed []time.Duration
	go func() {
		for {
			line, err := rwA.ReadString('\r')
			if err != nil {
				return
			}
			if strings.TrimSuffix(line, "\r") == "PTT ON" {
				aMu.Lock()
				aKeyed = append(aKeyed, time.Since(begin))
				aMu.Unlock()
			}
		}
	}()
	send(rwA, "CONNECT TESTA TESTB")

	const hold = 90 * time.Second // ARQ_ACCEPT_FALLBACK_HOLD_MS
	const foreignAt = 55 * time.Second
	const watch = 200 * time.Second
	var lines []string
	var acceptEnd, cancelled time.Duration
	keyed, foreignSent := false, false
	for time.Since(begin) < watch && cancelled == 0 {
		if acceptEnd > 0 && !foreignSent && time.Since(begin) >= acceptEnd+foreignAt {
			send(rwA, "CONNECT TESTA OTHER")
			foreignSent = true
			lines = append(lines, fmt.Sprintf("%5.1f (A calls OTHER)", time.Since(begin).Seconds()))
		}
		if err := connB.SetReadDeadline(time.Now().Add(500 * time.Millisecond)); err != nil {
			t.Fatal(err)
		}
		line, err := rwB.ReadString('\r')
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				continue
			}
			break
		}
		line = strings.TrimSuffix(line, "\r")
		at := time.Since(begin)
		switch {
		case line == "PENDING":
		case line == "PTT ON":
			// B's ACCEPT: it decoded A's CALL.  A gives up before its next.
			if !keyed {
				send(rwA, "DISCONNECT")
				keyed = true
			}
		case line == "PTT OFF":
			acceptEnd = at // B transmits only its ACCEPTs here
		case line == "CANCELPENDING":
			cancelled = at
		case strings.HasPrefix(line, "CONNECTED"):
			logs()
			t.Fatalf("B connected, A should never hear its ACCEPT:\n%s", strings.Join(lines, "\n"))
		default:
			continue
		}
		lines = append(lines, fmt.Sprintf("%5.1f %s", at.Seconds(), line))
	}
	send(rwA, "ABORT")
	if acceptEnd == 0 || cancelled == 0 || !foreignSent {
		logs()
		t.Fatalf("want B's ACCEPT, A's foreign CALL and a CANCELPENDING, got:\n%s", strings.Join(lines, "\n"))
	}
	aMu.Lock()
	inHold := false
	for _, k := range aKeyed {
		if k > acceptEnd+foreignAt-time.Second && k < acceptEnd+hold-5*time.Second {
			inHold = true
		}
	}
	aMu.Unlock()
	if !inHold {
		logs()
		t.Fatalf("A's foreign CALL did not key inside B's hold (%v after its ACCEPT):\n%s",
			hold, strings.Join(lines, "\n"))
	}
	if got := cancelled - acceptEnd; got < hold-5*time.Second {
		logs()
		t.Fatalf("CANCELPENDING %.1f s after B's last ACCEPT, inside the %v hold:\n%s",
			got.Seconds(), hold, strings.Join(lines, "\n"))
	}
	t.Logf("CANCELPENDING %.1f s after B's last ACCEPT; A's foreign CALL keyed inside the hold",
		(cancelled - acceptEnd).Seconds())
}

// drainFIFO reads and discards a station's TX audio, reopening the pipe
// whenever it has no writer.
func drainFIFO(ctx context.Context, path string) {
	var buf [4096]byte
	for ctx.Err() == nil {
		fd, err := waitForFIFOOpen(ctx, path, syscall.O_RDONLY|syscall.O_NONBLOCK)
		if err != nil {
			return
		}
		_ = syscall.SetNonblock(fd, false)
		f := os.NewFile(uintptr(fd), path)
		stop := context.AfterFunc(ctx, func() { f.Close() })
		for {
			n, err := f.Read(buf[:])
			if err != nil || n == 0 {
				break
			}
		}
		stop()
		f.Close()
		time.Sleep(5 * time.Millisecond)
	}
}

// silenceFIFO feeds a station's RX zeros; the pipe paces it to the reader.
func silenceFIFO(ctx context.Context, path string) {
	fd, err := waitForFIFOOpen(ctx, path, syscall.O_WRONLY|syscall.O_NONBLOCK)
	if err != nil {
		return
	}
	_ = syscall.SetNonblock(fd, false)
	f := os.NewFile(uintptr(fd), path)
	go func() { <-ctx.Done(); f.Close() }()
	var zeros [4096]byte
	for {
		if _, err := f.Write(zeros[:]); err != nil {
			return
		}
	}
}
