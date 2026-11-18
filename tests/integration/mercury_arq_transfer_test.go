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
// The callee hangs up to a caller that hears it only at the floor (return
// path below DATAC16): its DISCONNECT has to go on MFSK, where the caller
// listens, or the caller polls a closed session.  (The caller hanging up to a
// floor-only callee -- MERCURY_TEST_DISCONNECT_BY=A, NO_FWD=0, NO_REV=-24.82
// -- takes ~5 min at the floor, so it is left to be run by hand.)
func TestMercuryARQDisconnectReachesFloorPeer(t *testing.T) {
	t.Setenv("MERCURY_TEST_DISCONNECT_BY", "B")
	t.Setenv("MERCURY_CH_NO_FWD", "-24.82")
	t.Setenv("MERCURY_CH_NO_REV", "0")
	t.Setenv("MERCURY_TEST_PAYLOAD_B", "1024")
	TestMercuryARQTransfer(t)
}

// A caller and a callee at different bandwidths find each other on the MFSK
// CALL: B listens only after A's fast DATAC16 CALLs, so the connect has to go
// on A's MFSK CALL -- the floor of A's bandwidth, which B decodes only if it
// listens on both floors (#442: K7EK at BW2750 calling KG9NI at BW500, whose
// four MFSK CALLs went unheard).  B answers on the CALL's carrier.
func TestMercuryARQConnectWideCallerNarrowCallee(t *testing.T) {
	t.Setenv("MERCURY_TEST_BW_A", "2750")
	t.Setenv("MERCURY_TEST_BW_B", "500")
	t.Setenv("MERCURY_TEST_LISTEN_DELAY_S", "18")
	t.Setenv("MERCURY_TEST_PAYLOAD_B", "64")
	t.Setenv("MERCURY_TEST_EXPECT_LOG_B", "ACCEPT on the MFSK floor")
	TestMercuryARQTransfer(t)
}

// The other way round: a BW500 caller's MFSK CALL is the 500 Hz floor, which
// a BW2300 listener decodes only if it listens on both floors.
func TestMercuryARQConnectNarrowCallerWideCallee(t *testing.T) {
	t.Setenv("MERCURY_TEST_BW_A", "500")
	t.Setenv("MERCURY_TEST_BW_B", "2300")
	t.Setenv("MERCURY_TEST_LISTEN_DELAY_S", "18")
	t.Setenv("MERCURY_TEST_PAYLOAD_B", "64")
	t.Setenv("MERCURY_TEST_EXPECT_LOG_B", "ACCEPT on the MFSK floor")
	TestMercuryARQTransfer(t)
}

// The callee ABORTs at BW500: the host has its DISCONNECTED at once, and a
// scanning host (BPQ32) restarts its scan 15 s later, so the callee holds
// it with PENDING until its one DISCONNECT is out, then CANCELPENDING
// (arq.c, abort_hold).  On air (K7EK, 2026-11-13 15:17) the scan hopped
// 16.6 s after the ABORT while the caller's 26 s MFSK500 frame was on the
// air, the DISCONNECT was never sent, and the caller waited 240 s.
func TestMercuryARQCalleeAbortHoldsTheScan(t *testing.T) {
	t.Setenv("MERCURY_TEST_DISCONNECT_BY", "B")
	t.Setenv("MERCURY_TEST_ABORT", "1")
	t.Setenv("MERCURY_TEST_BW_A", "500")
	t.Setenv("MERCURY_TEST_BW_B", "500")
	t.Setenv("MERCURY_TEST_PAYLOAD_B", "64")
	TestMercuryARQTransfer(t)
}

// The caller ABORTs while one of its floor rounds is on the air: the radio
// unkeys within the tail, not at the round's end, and the ABORT's one
// DISCONNECT still reaches the callee (#592).  On air (VE4KLM, 2026-11-15
// 17:07:34, JNOS "mbox reset") a 27 s round played on 20 s past the ABORT.
// A->B at about -9 dB (SNR3k = -No - 14.82) keeps the caller's data on the
// MFSK floor; B->A is clean, so B's patterns and polls reach A.
func TestMercuryARQAbortCutsTheFloorRound(t *testing.T) {
	t.Setenv("MERCURY_TEST_ABORT_MIDROUND", "1")
	t.Setenv("MERCURY_CH_NO_FWD", "-5.82")
	t.Setenv("MERCURY_CH_NO_REV", "-24.82")
	t.Setenv("MERCURY_TEST_PAYLOAD_B", "1024")
	TestMercuryARQTransfer(t)
}

// The caller hangs up on the callee's last bytes, with nothing of its own
// left (a P2P host, a TNC probe): the ack it owed them goes before its
// DISCONNECT.  Before, the DISCONNECT took the ack's place and the callee
// logged "20 bytes of ours unconfirmed" with every byte delivered (#585: 7
// runs in 9 on this channel).
func TestMercuryARQCallerHangsUpOnCalleesLastBytes(t *testing.T) {
	t.Setenv("MERCURY_TEST_DISCONNECT_BY", "A")
	t.Setenv("MERCURY_TEST_REPLY_B", "20")
	t.Setenv("MERCURY_TEST_PAYLOAD_B", "20")
	TestMercuryARQTransfer(t)
}

func TestMercuryARQTransfer(t *testing.T) {
	repoRoot := mustRepoRoot(t)
	bin := locateOrBuildMercury(t, repoRoot)

	params := channelParamsFor(t)

	// Channel engine: codec2 ch.c (default), the Watterson HF model, or the
	// AE4JY pathsim reference (MERCURY_CH_ENGINE=pathsim, streaming-mode
	// binary pointed at by MERCURY_PATHSIM_BIN; MERCURY_CH_NO then carries the
	// pathsim-native --snr dB, and MERCURY_CH_FADING one of its presets, e.g.
	// ccir-poor or midlat-dist-nvis).  For watterson, MERCURY_CH_FADING is any
	// name from utils/watterson_test --list-presets, e.g. ccir-poor or itur-mm
	// (standard), or the legacy good/moderate/poor/flutter/mpg/mpp/mpd
	// (Butterworth taps).  The Watterson model gives controllable slow/deep
	// fades that reach the gear-shift oscillation regime ch --mpp cannot.
	var chBin string
	var err error
	switch os.Getenv("MERCURY_CH_ENGINE") {
	case "watterson":
		params.Engine = "watterson"
		chBin, err = buildWatterson(repoRoot)
	case "pathsim":
		params.Engine = "pathsim"
		chBin = os.Getenv("MERCURY_PATHSIM_BIN")
		if chBin == "" {
			t.Skip("MERCURY_CH_ENGINE=pathsim requires MERCURY_PATHSIM_BIN")
		}
	default:
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
	requireChFading(t, repoRoot, params)
	if v := os.Getenv("MERCURY_CH_GAIN"); v != "" {
		g, err := strconv.ParseFloat(v, 64)
		if err != nil {
			t.Fatalf("bad MERCURY_CH_GAIN %q: %v", v, err)
		}
		params.Gain = g
	}
	// MERCURY_CH_FREQ is the carrier offset in Hz: the transmitter's
	// frequency error as the receiver sees it, passed to the channel process
	// as --freq.  It is what the OFDM acquisition's capture window (+-55 Hz)
	// and the MFSK floor's (+-100 Hz) are measured against, so it is how a
	// session is run over a mistuned link.  See docs/ACQUISITION-WINDOWS.md.
	if v := os.Getenv("MERCURY_CH_FREQ"); v != "" {
		f, err := strconv.ParseFloat(v, 64)
		if err != nil {
			t.Fatalf("bad MERCURY_CH_FREQ %q: %v", v, err)
		}
		params.FreqOffsetHz = f
	}

	// MERCURY_TEST_TRANSPORT=sock runs the deterministic lockstep bench
	// instead of the real-time FIFO bridge: virtual time advances only as the
	// simulator hands out blocks, so a pinned seed reproduces a run exactly.
	// See socksim_test.go for why that matters at the fringe.
	useSock := os.Getenv("MERCURY_TEST_TRANSPORT") == "sock"

	dir := t.TempDir()
	aRX := filepath.Join(dir, "a_rx.s32le.fifo")
	aTX := filepath.Join(dir, "a_tx.s32le.fifo")
	bRX := filepath.Join(dir, "b_rx.s32le.fifo")
	bTX := filepath.Join(dir, "b_tx.s32le.fifo")
	// Unix socket paths are capped near 108 bytes, and t.TempDir() is long,
	// so the sock bench puts its sockets somewhere short.
	sockDir, err := os.MkdirTemp("", "msim")
	if err != nil {
		t.Fatalf("temp dir: %v", err)
	}
	defer os.RemoveAll(sockDir)
	sockA := filepath.Join(sockDir, "a.sock")
	sockB := filepath.Join(sockDir, "b.sock")

	if !useSock {
		for _, p := range []string{aRX, aTX, bRX, bTX} {
			if err := syscall.Mkfifo(p, 0600); err != nil {
				t.Fatalf("mkfifo %s: %v", p, err)
			}
		}
	}

	aPort := freePortPair(t)
	bPort := freePortPair(t)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	timingLogs := map[string]string{}
	startInstance := func(name, rxPath, txPath string, port, bcastPort int) (*exec.Cmd, *processWait, *os.File, *os.File) {
		stdout, stderr := tempLogFilesNamed(t, name)
		// -L writes a TIMING-level log FILE through the async logger: that is
		// where the per-session disconnect reason and the tx/ack/retry
		// timeline land.  They never reach the console at the default level,
		// and raising the console level perturbs the timing enough to hide
		// the races worth diagnosing.
		timing := filepath.Join(t.TempDir(), name+".timing.log")
		timingLogs[name] = timing
		transport := []string{"-x", "fifo", "-i", rxPath, "-o", txPath}
		if useSock {
			// The station connects to the simulator and takes its clock from
			// it; there is no separate playback path.
			transport = []string{"-x", "sock", "-i", rxPath}
		}
		args := append([]string{"-L", timing}, transport...)
		args = append(args,
			"-p", fmt.Sprint(port),
			"-b", fmt.Sprint(bcastPort),
			"-m", "1",
			"-C", filepath.Join(t.TempDir(), "missing-mercury.ini"),
		)
		// MERCURY_TEST_VERBOSE=1: -v on both stations, for diagnosis (it
		// perturbs timing, see -L above).
		if os.Getenv("MERCURY_TEST_VERBOSE") != "" {
			args = append(args, "-v")
		}
		// MERCURY_TEST_BIN_A / _B: run that station on another build (a
		// release, trunk), to see how mixed versions meet on the air.
		stationBin := bin
		if v := os.Getenv("MERCURY_TEST_BIN_" + name); v != "" {
			stationBin = v
		}
		cmd := exec.CommandContext(ctx, stationBin, args...)
		cmd.Dir = repoRoot
		cmd.Stdout = stdout
		cmd.Stderr = stderr
		if err := startChild(cmd); err != nil {
			t.Fatalf("start mercury %s: %v", name, err)
		}
		return cmd, waitForProcess(cmd), stdout, stderr
	}

	if useSock {
		fwd, rev := directionParams(params)
		sim, err := startSockSim(ctx, chBin, sockA, sockB, fwd, rev)
		if err != nil {
			t.Fatalf("start sock sim: %v", err)
		}
		defer func() {

			aS, aN, bS, bN := sim.BurstLevels()
			fmt.Printf("SOCKSIM burst/noise: A hears %.1f/%.1f dBFS (%.1f dB), B hears %.1f/%.1f dBFS (%.1f dB)\n",
				aS, aN, aS-aN, bS, bN, bS-bN)
			txA, txB := sim.TxLevels()
			fmt.Printf("SOCKSIM tx level (keyed): A=%.1f dBFS  B=%.1f dBFS\n", txA, txB)
			lvlA, lvlB := sim.RxLevels()
			fmt.Printf("SOCKSIM rx level: A=%.1f dBFS  B=%.1f dBFS\n", lvlA, lvlB)
			blocks, late, maxLate := sim.Cadence()
			fmt.Printf("SOCKSIM cadence: %d blocks (%.1fs of audio), %d late, worst %dms\n",
				blocks, float64(blocks)*sockBlockMs/1000, late, maxLate)
			sim.Close() // drains the channel models so their SNR report exists
			fmt.Printf("SOCKSIM measured SNR3k: %s\n", sim.ChannelSNR())
		}()
	} else {
		bridge := startChannelBridge(ctx, chBin, aTX, bRX, bTX, aRX, params)
		defer bridge.Close()
	}

	aIn, bIn := aRX, bRX
	if useSock {
		aIn, bIn = sockA, sockB
	}
	cmdA, procA, outA, errA := startInstance("A", aIn, aTX, aPort, aPort+100)
	defer func() { _ = stopProcess(t, cmdA, procA, outA.Name(), errA.Name()) }()
	cmdB, procB, outB, errB := startInstance("B", bIn, bTX, bPort, bPort+100)
	defer func() { _ = stopProcess(t, cmdB, procB, outB.Name(), errB.Name()) }()

	failWithLogs := func(format string, args ...interface{}) {
		printLogs(t, outA.Name(), errA.Name())
		printLogs(t, outB.Name(), errB.Name())
		printLogs(t, timingLogs["A"], timingLogs["B"])
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

	type setupCmd struct {
		conn       net.Conn
		rw         *bufio.ReadWriter
		cmd        string
		drainAfter string
	}
	setup := []setupCmd{
		{connA, rwA, "MYCALL TESTA", "REGISTERED TESTA"},
		{connB, rwB, "MYCALL TESTB", "REGISTERED TESTB"},
	}
	// MERCURY_TEST_BW=500|2300|2750: both stations' bandwidth (the TNC BW
	// command), so the session negotiates it -- e.g. to check that a BW500
	// session never keys a wideband rung.
	if bw := os.Getenv("MERCURY_TEST_BW"); bw != "" {
		setup = append(setup, setupCmd{connA, rwA, "BW" + bw, ""}, setupCmd{connB, rwB, "BW" + bw, ""})
	}
	// MERCURY_TEST_BW_A / MERCURY_TEST_BW_B: one station's bandwidth, over
	// MERCURY_TEST_BW -- stations configured differently, as on air (#442:
	// a BW2750 caller, a BW500 callee).
	if bw := os.Getenv("MERCURY_TEST_BW_A"); bw != "" {
		setup = append(setup, setupCmd{connA, rwA, "BW" + bw, ""})
	}
	if bw := os.Getenv("MERCURY_TEST_BW_B"); bw != "" {
		setup = append(setup, setupCmd{connB, rwB, "BW" + bw, ""})
	}
	// MERCURY_TEST_LISTEN_DELAY_S=<s>: B starts listening that long after A's
	// CONNECT, so A's fast CALLs go unheard and the connect goes on MFSK on a
	// channel where the control mode works (a busy or late callee).
	listenDelay := 0.0
	if v := os.Getenv("MERCURY_TEST_LISTEN_DELAY_S"); v != "" {
		if listenDelay, err = strconv.ParseFloat(v, 64); err != nil {
			t.Fatalf("bad MERCURY_TEST_LISTEN_DELAY_S %q", v)
		}
	}
	if listenDelay <= 0 {
		setup = append(setup, setupCmd{connB, rwB, "LISTEN ON", ""})
	}
	for _, c := range setup {
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

	tConnect := time.Now()
	if got := sendControlCommand(t, connA, rwA, "CONNECT TESTA TESTB"); !strings.HasPrefix(got, "OK") {
		failWithLogs("CONNECT -> %q, want OK", got)
	}
	listenErr := make(chan error, 1)
	if listenDelay > 0 {
		go func() {
			time.Sleep(time.Duration(listenDelay * float64(time.Second)))
			_ = connB.SetDeadline(time.Now().Add(commandTimeout))
			_, err := rwB.WriteString("LISTEN ON\r")
			if err == nil {
				err = rwB.Flush()
			}
			// The reply, past B's unsolicited status lines (BUFFER 0): B
			// cannot be CONNECTED before it listens, so nothing else is lost.
			for err == nil {
				var line string
				if line, err = rwB.ReadString('\r'); err != nil {
					break
				}
				if strings.HasPrefix(line, "OK") {
					break
				}
				if strings.HasPrefix(line, "WRONG") {
					err = fmt.Errorf("LISTEN ON -> %q", line)
				}
			}
			_ = connB.SetDeadline(time.Time{})
			listenErr <- err
		}()
	} else {
		listenErr <- nil
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
	t.Logf("CONNECTED after %.1f s", time.Since(tConnect).Seconds())
	// MERCURY_TEST_EXPECT_LOG_B=<text>: B's log must say it (how the connect
	// went, e.g. on which carrier B answered).  The async logger writes it
	// within a moment of the event.
	if want := os.Getenv("MERCURY_TEST_EXPECT_LOG_B"); want != "" {
		found := false
		for end := time.Now().Add(2 * time.Second); time.Now().Before(end) && !found; time.Sleep(100 * time.Millisecond) {
			b, _ := os.ReadFile(timingLogs["B"])
			found = strings.Contains(string(b), want)
		}
		if !found {
			failWithLogs("B's log never said %q", want)
		}
	}
	_ = connA.SetReadDeadline(time.Time{})
	if err := <-listenErr; err != nil {
		failWithLogs("B's delayed LISTEN ON: %v", err)
	}

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
	// Exact byte count, which MERCURY_TEST_PAYLOAD_KB cannot express.  A
	// like-for-like A/B across the SNR range needs ONE payload at every point.
	if v := os.Getenv("MERCURY_TEST_PAYLOAD_B"); v != "" {
		nb, err := strconv.Atoi(v)
		if err != nil || nb <= 0 {
			t.Fatalf("bad MERCURY_TEST_PAYLOAD_B %q", v)
		}
		unit := "MERCURY-DATAC15-PAYLOAD-0123456789"
		payload = []byte(strings.Repeat(unit, nb/len(unit)+1))[:nb]
	}
	// MERCURY_TEST_PAYLOAD_RANDOM=1: incompressible bytes from a fixed PRNG, so
	// the throughput measured is the link's raw rate (a repeated string
	// compresses to a few frames and overstates it).  Same bytes every run.
	if os.Getenv("MERCURY_TEST_PAYLOAD_RANDOM") != "" {
		var x uint32 = 0x9E3779B9
		for i := range payload {
			x ^= x << 13
			x ^= x >> 17
			x ^= x << 5
			payload[i] = byte(x)
		}
	}
	if _, err := dataA.Write(payload); err != nil {
		failWithLogs("write payload: %v", err)
	}

	// MERCURY_TEST_ABORT_MIDROUND=1: A ABORTs 3 s into a floor keydown of
	// its own (an MFSK BITRATE, then PTT ON, and still keyed 3 s on), which
	// must unkey within the tail plus a margin -- not play on to its end --
	// and B must still hear the ABORT's DISCONNECT.  Nothing is delivered.
	if os.Getenv("MERCURY_TEST_ABORT_MIDROUND") != "" {
		const into = 3 * time.Second
		const unkeyBy = 2500 * time.Millisecond // TAIL_TIME_US is 100 ms
		searchEnd := time.Now().Add(8 * time.Minute)
		var partial string
		// readA: one control line, or "" at the deadline (a partial line is
		// kept for the next call).
		readA := func(by time.Time) (string, bool) {
			_ = connA.SetReadDeadline(by)
			s, err := rwA.ReadString('\r')
			partial += s
			if err != nil {
				if ne, ok := err.(net.Error); ok && ne.Timeout() {
					return "", true
				}
				failWithLogs("A's control port: %v", err)
			}
			line := strings.TrimSpace(partial)
			partial = ""
			return line, false
		}
		floor, keyedAt := false, time.Time{}
		for {
			if time.Now().After(searchEnd) {
				failWithLogs("A keyed no floor round 3 s long in 8 min")
			}
			by := searchEnd
			if !keyedAt.IsZero() {
				by = keyedAt.Add(into)
			}
			line, timedOut := readA(by)
			if timedOut {
				if !keyedAt.IsZero() {
					break // still keyed 3 s into a floor keydown
				}
				continue
			}
			switch {
			case strings.HasPrefix(line, "BITRATE (100)") || strings.HasPrefix(line, "BITRATE (103)"):
				floor = true
			case strings.HasPrefix(line, "BITRATE"):
				floor = false
			case line == "PTT ON" && floor:
				keyedAt = time.Now()
			case line == "PTT OFF":
				keyedAt = time.Time{}
			case strings.HasPrefix(line, "DISCONNECTED"):
				failWithLogs("A disconnected before a floor round: %q", line)
			}
		}
		t.Logf("A keyed a floor round %.1f s ago: ABORT", time.Since(keyedAt).Seconds())
		_ = connA.SetWriteDeadline(time.Now().Add(5 * time.Second))
		if _, err := rwA.WriteString("ABORT\r"); err != nil {
			t.Fatalf("write ABORT: %v", err)
		}
		if err := rwA.Flush(); err != nil {
			t.Fatalf("flush ABORT: %v", err)
		}
		tAbort := time.Now()
		var after []string
		for {
			line, timedOut := readA(tAbort.Add(30 * time.Second))
			if timedOut {
				failWithLogs("A never unkeyed after its ABORT: %q", after)
			}
			after = append(after, line)
			if line == "PTT OFF" {
				break
			}
		}
		unkeyed := time.Since(tAbort)
		t.Logf("A unkeyed %.2f s after its ABORT (%.1f s into the round): %q",
			unkeyed.Seconds(), time.Since(keyedAt).Seconds()-unkeyed.Seconds(), after)
		if unkeyed > unkeyBy {
			failWithLogs("A unkeyed %.2f s after its ABORT, want within %.1f s", unkeyed.Seconds(), unkeyBy.Seconds())
		}
		if b, _ := os.ReadFile(timingLogs["A"]); !strings.Contains(string(b), "keydown cut short (ABORT)") {
			failWithLogs("A's log never said its keydown was cut")
		}
		deadline := time.Now().Add(4 * time.Minute)
		_ = connB.SetReadDeadline(deadline)
		heard := false
		for time.Now().Before(deadline) && !heard {
			line, err := rwB.ReadString('\r')
			if err != nil {
				break
			}
			heard = strings.HasPrefix(strings.TrimSpace(line), "DISCONNECTED")
		}
		if !heard {
			failWithLogs("B never heard A's DISCONNECT after the cut round")
		}
		t.Logf("B heard A's DISCONNECT %.1f s after the ABORT", time.Since(tAbort).Seconds())
		return
	}

	rx := make([]byte, 0, len(payload))
	buf := make([]byte, 4096)
	// Scale on the SLOWEST rung, not a nominal one: MFSK carries 61 bit/s
	// (~7.6 B/s), so the old 100 B/s allowance timed out a perfectly healthy
	// fringe transfer long before it could finish.  5 B/s is a floor beneath
	// every mode in the ladder, so this bounds a genuine stall without
	// failing a link that is merely slow because the channel is bad.
	rxDeadline := time.Now().Add(5*time.Minute +
		time.Duration(len(payload)/5)*time.Second)
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

	// MERCURY_TEST_REPLY_B=<n>: B answers with n bytes once it has A's, and
	// A has them before it hangs up (MERCURY_TEST_DISCONNECT_BY=A) with no
	// last word of its own: B's last bytes are the session's last, and B
	// must not log them unconfirmed (#585).
	replyB := 0
	if v := os.Getenv("MERCURY_TEST_REPLY_B"); v != "" {
		if replyB, err = strconv.Atoi(v); err != nil || replyB <= 0 {
			t.Fatalf("bad MERCURY_TEST_REPLY_B %q", v)
		}
		reply := []byte(strings.Repeat("R", replyB))
		if _, err := dataB.Write(reply); err != nil {
			failWithLogs("B write reply: %v", err)
		}
		var got []byte
		for end := time.Now().Add(3 * time.Minute); len(got) < replyB && time.Now().Before(end); {
			_ = dataA.SetReadDeadline(time.Now().Add(15 * time.Second))
			n, err := dataA.Read(buf)
			got = append(got, buf[:n]...)
			if err != nil {
				if ne, ok := err.(net.Error); ok && ne.Timeout() {
					continue
				}
				break
			}
		}
		if string(got) != string(reply) {
			failWithLogs("A got %d of B's %d reply bytes", len(got), replyB)
		}
		_ = dataA.SetReadDeadline(time.Time{})
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
	// MERCURY_TEST_DISCONNECT_BY=A|B: that side hangs up after the transfer,
	// and the other must hear it (DISCONNECTED on its control port).  Over an
	// asymmetric link (MERCURY_CH_NO_FWD/REV) a DISCONNECT the peer cannot
	// decode leaves it polling a closed session: on air a gateway at 2 %
	// sent its DISCONNECTs on DATAC16 to an estacao2 that heard it only on
	// MFSK, and estacao2 polled until its uucico was killed.
	if by := os.Getenv("MERCURY_TEST_DISCONNECT_BY"); by == "A" || by == "B" {
		hang, hangRW, other, otherRW, name := connA, rwA, connB, rwB, "B"
		hangData, otherData := dataA, dataB
		if by == "B" {
			hang, hangRW, other, otherRW, name = connB, rwB, connA, rwA, "A"
			hangData, otherData = dataB, dataA
		}
		// A last word written just before the DISCONNECT, as a Winlink client
		// sends FQ and hangs up: it must reach the peer.  Data and command
		// travel on separate sockets, and on air the DISCONNECT overtook it
		// and the session ended with the word dropped.
		lastWord := []byte("LAST-WORD-0123")
		// MERCURY_TEST_ABORT=1: ABORT instead, which drops what is undelivered,
		// so no last word; the host's lines after it are checked instead.
		abort := os.Getenv("MERCURY_TEST_ABORT") != ""
		hangCmd := "DISCONNECT"
		if abort {
			hangCmd = "ABORT"
		} else if replyB > 0 {
			lastWord = nil // hanging up on the peer's last bytes, nothing of its own
		} else if _, err := hangData.Write(lastWord); err != nil {
			t.Fatalf("write last word: %v", err)
		}
		// Not sendControlCommand: the control port is busy with notifications
		// (BUFFER, PTT, ...) by now, and the first line need not be the reply.
		if err := hang.SetDeadline(time.Now().Add(15 * time.Second)); err != nil {
			t.Fatal(err)
		}
		if _, err := hangRW.WriteString(hangCmd + "\r"); err != nil {
			t.Fatalf("write %s: %v", hangCmd, err)
		}
		if err := hangRW.Flush(); err != nil {
			t.Fatalf("flush %s: %v", hangCmd, err)
		}
		var after []string // the host's lines from the reply on
		for {
			line, err := hangRW.ReadString('\r')
			if err != nil {
				failWithLogs("no OK for %s: %v", hangCmd, err)
			}
			if line = strings.TrimSpace(line); strings.HasPrefix(line, "OK") {
				break
			}
			after = append(after, line)
		}
		if abort {
			// DISCONNECTED, then PENDING (the scan held), then CANCELPENDING
			// once the one DISCONNECT is out: never PENDING before
			// DISCONNECTED, which would clear it in BPQ32.
			_ = hang.SetDeadline(time.Now().Add(90 * time.Second))
			// The lines before it are the session's own (the connect's
			// PENDING among them): look from the ABORT's DISCONNECTED on.
			idx := func(want string, from int) int {
				for i := from; i >= 0 && i < len(after); i++ {
					if after[i] == want {
						return i
					}
				}
				return -1
			}
			done := func() bool {
				d := idx("DISCONNECTED", 0)
				p := idx("PENDING", d+1)
				return d >= 0 && p >= 0 && idx("CANCELPENDING", p+1) >= 0
			}
			for !done() {
				line, err := hangRW.ReadString('\r')
				if err != nil {
					break
				}
				after = append(after, strings.TrimSpace(line))
			}
			if !done() {
				failWithLogs("%s's host after ABORT: want DISCONNECTED, then PENDING, then CANCELPENDING, got %q",
					by, after)
			}
			t.Logf("%s's host after ABORT: %q", by, after)
		}
		_ = hang.SetDeadline(time.Time{})
		t0 := time.Now()
		deadline := t0.Add(4 * time.Minute)
		if err := other.SetReadDeadline(deadline); err != nil {
			t.Fatal(err)
		}
		heard := false
		for time.Now().Before(deadline) && !heard {
			line, err := otherRW.ReadString('\r')
			if err != nil {
				break
			}
			heard = strings.HasPrefix(strings.TrimSpace(line), "DISCONNECTED")
		}
		if !heard {
			failWithLogs("%s never heard %s's DISCONNECT", name, by)
		}
		if abort {
			t.Logf("%s heard %s's DISCONNECT after %.1f s", name, by, time.Since(t0).Seconds())
			return
		}
		var got []byte
		_ = otherData.SetReadDeadline(time.Now().Add(5 * time.Second))
		for len(got) < len(lastWord) {
			n, err := otherData.Read(buf)
			got = append(got, buf[:n]...)
			if err != nil {
				break
			}
		}
		if string(got) != string(lastWord) {
			failWithLogs("%s got %q of %s's last word %q", name, got, by, lastWord)
		}
		t.Logf("%s heard %s's DISCONNECT after %.1f s", name, by, time.Since(t0).Seconds())
		if replyB > 0 {
			// The async logger writes the warning within a moment of the end.
			time.Sleep(time.Second)
			b, _ := os.ReadFile(timingLogs[name])
			if strings.Contains(string(b), "bytes of ours unconfirmed") {
				failWithLogs("%s logged its last bytes unconfirmed, though %s had them", name, by)
			}
		}
	}
	engine := params.Engine
	if engine == "" {
		engine = "ch"
	}
	t.Logf("ARQ transfer complete over %s (No=%.1f dB): %d bytes delivered", engine, params.No_dBHz, len(payload))
}
