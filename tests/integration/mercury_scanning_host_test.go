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
	"sync"
	"syscall"
	"testing"
	"time"
)

// A gateway whose host scans.  RMS Trimode stays on a frequency for 4 s, is
// deaf for 0.5 s while it changes, and leaves for the next one unless the TNC
// says PENDING (#527).  A CALL is a 3.74 s frame, so one that is decoded
// before the host has gone almost never fits: the PENDING came when the frame
// had decoded.  Mercury now says it when a decoder locks onto the frame
// (arq.c, arrival_hold), about a second in, so a stay that catches the start
// of a CALL keeps the channel for the rest of it.
//
// A calls B.  B's audio, A's CALLs, reaches B only while B's host is on the
// channel: the host takes the first 4 s of every 4.5 s x channels, the first
// 0.5 s of each stay deaf, and stays on a PENDING until a CANCELPENDING, a
// CONNECTED or a DISCONNECTED (BPQ32 gives up on a PENDING after six scan
// intervals, 60 s).  The audio is paced in real time, as a radio's is.
//
// MERCURY_TEST_SCAN_CHANNELS (default 2) and MERCURY_TEST_SCAN_PHASE (seconds,
// default 0) set the schedule.  MERCURY_TEST_SCAN_LIMIT_S is how long A may
// try: 90 s by default, which phase 0 needs 19.4 s of; the 9-phase table in
// the PR ran with 120, within which phase 0 never connects without the hold.
// MERCURY_TEST_BIN_B runs another build as the gateway: an older Mercury
// does not connect here at phase 0, which is the point.
func TestScanningHostIsHeldByAnArrivingCall(t *testing.T) {
	repoRoot := mustRepoRoot(t)
	binA := locateOrBuildMercury(t, repoRoot)
	binB := binA
	if env := os.Getenv("MERCURY_TEST_BIN_B"); env != "" {
		binB = env
	}

	envNum := func(name string, def float64) float64 {
		if v := os.Getenv(name); v != "" {
			f, err := strconv.ParseFloat(v, 64)
			if err != nil {
				t.Fatalf("bad %s %q", name, v)
			}
			return f
		}
		return def
	}
	channels := int(envNum("MERCURY_TEST_SCAN_CHANNELS", 2))
	phase := time.Duration(envNum("MERCURY_TEST_SCAN_PHASE", 0) * float64(time.Second))
	limit := time.Duration(envNum("MERCURY_TEST_SCAN_LIMIT_S", 90) * float64(time.Second))
	const dwell, deaf = 4000 * time.Millisecond, 500 * time.Millisecond
	cycle := time.Duration(channels) * (dwell + deaf)

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

	start := func(bin, name, rx, tx string, port int) (*exec.Cmd, *processWait, *os.File, *os.File) {
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
	cmdA, procA, outA, errA := start(binA, "A", aRX, aTX, aPort)
	defer func() { _ = stopProcess(t, cmdA, procA, outA.Name(), errA.Name()) }()
	cmdB, procB, outB, errB := start(binB, "B", bRX, bTX, bPort)
	defer func() { _ = stopProcess(t, cmdB, procB, outB.Name(), errB.Name()) }()
	logs := func() {
		printLogs(t, outA.Name(), errA.Name())
		printLogs(t, outB.Name(), errB.Name())
	}

	// The host: on the channel in its window or while it holds a PENDING.
	var (
		mu       sync.Mutex
		holdTill time.Time // a PENDING's hold; zero: none
		stays    bool      // connected: it stays
	)
	t0 := time.Now()
	onChannel := func() (open bool, stay bool) {
		mu.Lock()
		defer mu.Unlock()
		now := time.Now()
		if stays || now.Before(holdTill) {
			return true, true
		}
		pos := (now.Sub(t0) + phase) % cycle
		return pos < dwell && pos >= deaf, false
	}
	go pacedBridge(ctx, aTX, bRX, func() bool { o, _ := onChannel(); return o })
	go pacedBridge(ctx, bTX, aRX, func() bool { return true })

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
	setup(connA, rwA, "TESTA", "RETRIES 40")
	setup(connB, rwB, "TESTB", "LISTEN ON")
	_ = connA.SetDeadline(time.Time{})
	_ = connB.SetDeadline(time.Time{})

	var lines []string
	var tl sync.Mutex
	note := func(who, line string) {
		tl.Lock()
		lines = append(lines, fmt.Sprintf("%6.1f %s %s", time.Since(t0).Seconds(), who, line))
		tl.Unlock()
	}
	timeline := func() string {
		tl.Lock()
		defer tl.Unlock()
		return strings.Join(lines, "\n")
	}

	// B's host reads its TNC.
	go func() {
		for {
			line, err := rwB.ReadString('\r')
			if err != nil {
				return
			}
			line = strings.TrimSuffix(line, "\r")
			switch {
			case line == "PENDING":
				mu.Lock()
				holdTill = time.Now().Add(60 * time.Second)
				mu.Unlock()
			case line == "CANCELPENDING" || line == "DISCONNECTED":
				mu.Lock()
				holdTill = time.Time{}
				stays = false
				mu.Unlock()
			case strings.HasPrefix(line, "CONNECTED"):
				mu.Lock()
				stays = true
				mu.Unlock()
			}
			if strings.HasPrefix(line, "PENDING") || strings.HasPrefix(line, "CANCEL") ||
				strings.HasPrefix(line, "CONNECTED") || strings.HasPrefix(line, "DISCONNECTED") {
				note("B", line)
			}
		}
	}()

	connected := make(chan struct{})
	go func() {
		for {
			line, err := rwA.ReadString('\r')
			if err != nil {
				return
			}
			line = strings.TrimSuffix(line, "\r")
			if strings.HasPrefix(line, "CONNECTED") {
				note("A", line)
				close(connected)
				return
			}
			if strings.HasPrefix(line, "DISCONNECTED") {
				note("A", line)
			}
		}
	}()

	if _, err := rwA.WriteString("CONNECT TESTA TESTB\r"); err != nil {
		t.Fatal(err)
	}
	if err := rwA.Flush(); err != nil {
		t.Fatal(err)
	}
	begin := time.Now()
	select {
	case <-connected:
		t.Logf("connected %.1f s after the CONNECT (%d channels, phase %.1f s)\n%s",
			time.Since(begin).Seconds(), channels, phase.Seconds(), timeline())
	case <-time.After(limit):
		logs()
		t.Fatalf("not connected within %v (%d channels, phase %.1f s):\n%s",
			limit, channels, phase.Seconds(), timeline())
	}
}

// pacedBridge copies one station's playback to another's capture in real
// time, 100 ms at a time, the way a radio carries audio: silence when the
// sender is quiet, and while open() says no, silence in place of the audio.
// A sender that writes faster than real time is held by the full pipe.
func pacedBridge(ctx context.Context, txPath, rxPath string, open func() bool) {
	rxFD, err := waitForFIFOOpen(ctx, rxPath, syscall.O_WRONLY|syscall.O_NONBLOCK)
	if err != nil {
		return
	}
	_ = syscall.SetNonblock(rxFD, false)
	rxF := os.NewFile(uintptr(rxFD), rxPath)
	txFD, err := waitForFIFOOpen(ctx, txPath, syscall.O_RDONLY|syscall.O_NONBLOCK)
	if err != nil {
		rxF.Close()
		return
	}
	txF := os.NewFile(uintptr(txFD), txPath)
	go func() { <-ctx.Done(); txF.Close(); rxF.Close() }()

	const chunk = 3200 // 100 ms of s32le at 8 kHz
	var acc []byte
	buf := make([]byte, 8192)
	tick := time.NewTicker(100 * time.Millisecond)
	defer tick.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-tick.C:
		}
		for len(acc) < chunk*4 {
			n, err := syscall.Read(txFD, buf)
			if n <= 0 || err != nil {
				break
			}
			acc = append(acc, buf[:n]...)
		}
		out := make([]byte, chunk)
		n := len(acc) / 4 * 4
		if n > chunk {
			n = chunk
		}
		if n > 0 {
			if open() {
				copy(out, acc[:n])
			}
			acc = acc[n:]
		}
		if _, err := rxF.Write(out); err != nil {
			return
		}
	}
}
