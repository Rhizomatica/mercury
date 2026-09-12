//go:build linux

package integration

// A real NNCP bundle carried over Mercury's one-way broadcast plane, between
// two Mercury modems with a channel simulator standing in for the propagation
// path.
//
// This is a TRIAL harness, not a regression test: it is skipped unless
// MERCURY_BCAST_TRIAL=1, because a single run at a robust mode takes minutes
// and the point is to sweep it, not to gate CI.
//
// What makes it worth running rather than just copying a file: the bundle is
// produced by nncp-bundle and consumed by nncp-bundle -rx + nncp-toss on a
// second node, so a pass means NNCP itself accepted what came off the air --
// signatures and all -- not merely that some bytes arrived intact.
//
// Knobs (all optional):
//   MERCURY_BCAST_MODE     hermes mode index, default 10 (QAM16C2)
//   MERCURY_BCAST_CYCLES   carousel cycles, default = 2x the computed need
//   MERCURY_BCAST_PAYLOAD  payload bytes fed to nncp-file, default 6000
//   MERCURY_BCAST_DEADLINE_S  how long to wait for arrival, default 300
//   MERCURY_CH_NO / _FADING / _ENGINE / _GAIN   as the other integration tests

import (
	"bufio"
	"context"
	"crypto/rand"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

// bcastFrameSize mirrors bcast_frame_size[] in datalink_broadcast/bcast_modes.h.
var bcastFrameSize = []int{510, 126, 14, 54, 14, 3, 30, 30, 14, 1180, 1213}

const bcastFrameOverhead = 12
const bcastSymbolSize = 41

func nncpAvailable() bool {
	for _, b := range []string{"nncp-cfgnew", "nncp-file", "nncp-bundle", "nncp-toss"} {
		if _, err := exec.LookPath(b); err != nil {
			return false
		}
	}
	return true
}

// nncpKeys runs nncp-cfgnew and pulls the self block's keys out of it.
func nncpKeys(t *testing.T) map[string]string {
	t.Helper()
	out, err := exec.Command("nncp-cfgnew").Output()
	if err != nil {
		t.Fatalf("nncp-cfgnew: %v", err)
	}
	blk := regexp.MustCompile(`(?s)\n  self: \{(.*?)\n  \}`).FindSubmatch(out)
	if blk == nil {
		t.Fatalf("cannot find self block in nncp-cfgnew output")
	}
	m := map[string]string{}
	for _, kv := range regexp.MustCompile(`(\w+):\s*(\S+)`).FindAllStringSubmatch(string(blk[1]), -1) {
		m[kv[1]] = kv[2]
	}
	return m
}

// writeNNCPCfg emits a minimal, fully-populated node config.  Generated rather
// than patched from nncp-cfgnew's template: that template is mostly comments,
// and regex surgery on it produces configs that parse but are subtly wrong.
func writeNNCPCfg(t *testing.T, root, name string, me, other map[string]string, otherName string) string {
	t.Helper()
	base := filepath.Join(root, name)
	for _, sub := range []string{"spool", "incoming"} {
		if err := os.MkdirAll(filepath.Join(base, sub), 0700); err != nil {
			t.Fatal(err)
		}
	}
	pub := func(d map[string]string) string {
		return fmt.Sprintf("id: %s\n      exchpub: %s\n      signpub: %s\n      noisepub: %s",
			d["id"], d["exchpub"], d["signpub"], d["noisepub"])
	}
	var priv []string
	for _, k := range []string{"id", "exchpub", "exchprv", "signpub", "signprv", "noiseprv", "noisepub"} {
		priv = append(priv, fmt.Sprintf("%s: %s", k, me[k]))
	}
	cfg := fmt.Sprintf(`{
  spool: %s/spool
  log: %s/log
  self: {
    %s
  }
  neigh: {
    self: {
      %s
    }
    %s: {
      %s
      incoming: "%s/incoming"
    }
  }
}
`, base, base, strings.Join(priv, "\n    "), pub(me), otherName, pub(other), base)
	p := filepath.Join(root, name+".hjson")
	if err := os.WriteFile(p, []byte(cfg), 0600); err != nil {
		t.Fatal(err)
	}
	return p
}

func TestBroadcastNNCPBundleOverChannel(t *testing.T) {
	if os.Getenv("MERCURY_BCAST_TRIAL") != "1" {
		t.Skip("broadcast NNCP trial: set MERCURY_BCAST_TRIAL=1 to run")
	}
	if !nncpAvailable() {
		t.Skip("nncp tools not installed")
	}
	repoRoot := mustRepoRoot(t)
	bin := locateOrBuildMercury(t, repoRoot)

	mode := 10
	if v := os.Getenv("MERCURY_BCAST_MODE"); v != "" {
		mode, _ = strconv.Atoi(v)
	}
	if mode < 0 || mode >= len(bcastFrameSize) {
		t.Fatalf("bad mode %d", mode)
	}
	symsPerFrame := (bcastFrameSize[mode] - bcastFrameOverhead) / bcastSymbolSize
	if symsPerFrame < 1 {
		t.Fatalf("mode %d cannot carry broadcast file transfer", mode)
	}
	payloadBytes := 6000
	if v := os.Getenv("MERCURY_BCAST_PAYLOAD"); v != "" {
		payloadBytes, _ = strconv.Atoi(v)
	}

	// --- build the channel -------------------------------------------------
	params := DefaultChannelParams()
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
		params.No_dBHz, _ = strconv.ParseFloat(v, 64)
	}
	if v := os.Getenv("MERCURY_CH_FADING"); v != "" {
		params.Fading = v
	}
	requireChFading(t, repoRoot, params.Fading)
	if v := os.Getenv("MERCURY_CH_GAIN"); v != "" {
		params.Gain, _ = strconv.ParseFloat(v, 64)
	}

	// --- build the bcast_file_tool ----------------------------------------
	tool := filepath.Join(repoRoot, "utils", "bcast_file_tool")
	build := exec.Command("make", "bcast_file_tool")
	build.Dir = filepath.Join(repoRoot, "utils")
	if out, berr := build.CombinedOutput(); berr != nil {
		t.Skipf("bcast_file_tool unavailable: %v\n%s", berr, out)
	}

	// --- a genuine two-node NNCP setup, and a real bundle ------------------
	nroot := t.TempDir()
	a, b := nncpKeys(t), nncpKeys(t)
	aCfg := writeNNCPCfg(t, nroot, "alice", a, b, "bob")
	bCfg := writeNNCPCfg(t, nroot, "bob", b, a, "alice")

	payload := filepath.Join(nroot, "payload.bin")
	buf := make([]byte, payloadBytes)
	if _, rerr := rand.Read(buf); rerr != nil {
		t.Fatalf("payload: %v", rerr)
	}
	if werr := os.WriteFile(payload, buf, 0600); werr != nil {
		t.Fatal(werr)
	}
	if out, ferr := exec.Command("nncp-file", "-cfg", aCfg, "-quiet", payload, "bob:payload.bin").CombinedOutput(); ferr != nil {
		t.Fatalf("nncp-file: %v\n%s", ferr, out)
	}
	bundle := filepath.Join(nroot, "out.nncp")
	bf, _ := os.Create(bundle)
	bcmd := exec.Command("nncp-bundle", "-cfg", aCfg, "-quiet", "-tx", "bob")
	bcmd.Stdout = bf
	if berr := bcmd.Run(); berr != nil {
		t.Fatalf("nncp-bundle -tx: %v", berr)
	}
	bf.Close()
	bst, _ := os.Stat(bundle)
	bundleSize := int(bst.Size())

	symsNeeded := bundleSize/bcastSymbolSize + 2
	framesNeeded := (symsNeeded + symsPerFrame - 1) / symsPerFrame
	cycles := framesNeeded * 2
	if v := os.Getenv("MERCURY_BCAST_CYCLES"); v != "" {
		cycles, _ = strconv.Atoi(v)
	}

	t.Logf("trial: mode=%d (%d B/frame, %d sym/frame)  payload=%d B  bundle=%d B",
		mode, bcastFrameSize[mode], symsPerFrame, payloadBytes, bundleSize)
	t.Logf("trial: need ~%d symbols = %d frames/cycle-set; sending cycles=%d",
		symsNeeded, framesNeeded, cycles)
	t.Logf("trial: channel engine=%q No=%.2f fading=%q gain=%.3f",
		params.Engine, params.No_dBHz, params.Fading, params.Gain)

	// --- two mercuries over the channel ------------------------------------
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

	start := func(name, rxPath, txPath string, port, bcastPort int) (*exec.Cmd, *processWait, *os.File, *os.File) {
		stdout, stderr := tempLogFilesNamed(t, name)
		cmd := exec.CommandContext(ctx, bin,
			"-x", "fifo", "-i", rxPath, "-o", txPath,
			"-p", fmt.Sprint(port), "-b", fmt.Sprint(bcastPort),
			"-m", fmt.Sprint(mode), "-v",
			"-C", filepath.Join(t.TempDir(), "missing-mercury.ini"),
		)
		cmd.Dir = repoRoot
		cmd.Stdout = stdout
		cmd.Stderr = stderr
		if serr := startChild(cmd); serr != nil {
			t.Fatalf("start mercury %s: %v", name, serr)
		}
		return cmd, waitForProcess(cmd), stdout, stderr
	}

	aBcast := aPort + 100
	bBcast := bPort + 100
	cmdA, procA, outA, errA := start("A-tx", aRX, aTX, aPort, aBcast)
	defer func() { _ = stopProcess(t, cmdA, procA, outA.Name(), errA.Name()) }()
	cmdB, procB, outB, errB := start("B-rx", bRX, bTX, bPort, bBcast)
	defer func() { _ = stopProcess(t, cmdB, procB, outB.Name(), errB.Name()) }()

	bridge := startChannelBridge(ctx, chBin, aTX, bRX, bTX, aRX, params)
	defer bridge.Close()

	// Control ports up = both instances are alive and past init.
	if c, cerr := waitForTCP(ctx, "127.0.0.1", aPort, controlPortTimeout, procA); cerr != nil {
		printLogs(t, outA.Name(), errA.Name())
		t.Fatalf("mercury A control port: %v", cerr)
	} else {
		c.Close()
	}
	if c, cerr := waitForTCP(ctx, "127.0.0.1", bPort, controlPortTimeout, procB); cerr != nil {
		printLogs(t, outB.Name(), errB.Name())
		t.Fatalf("mercury B control port: %v", cerr)
	} else {
		c.Close()
	}

	// --- receiver first, then transmit -------------------------------------
	recvDir := filepath.Join(nroot, "offair")
	if mkerr := os.MkdirAll(recvDir, 0700); mkerr != nil {
		t.Fatal(mkerr)
	}
	recvCtx, recvCancel := context.WithCancel(ctx)
	defer recvCancel()
	recv := exec.CommandContext(recvCtx, tool, "recv", recvDir,
		"-m", fmt.Sprint(mode), "-p", fmt.Sprint(bBcast))
	recvOut, _ := recv.StdoutPipe()
	recv.Stderr = recv.Stdout
	if rerr := recv.Start(); rerr != nil {
		t.Fatalf("start recv: %v", rerr)
	}
	recvLines := make(chan string, 256)
	go func() {
		sc := bufio.NewScanner(recvOut)
		for sc.Scan() {
			recvLines <- sc.Text()
		}
		close(recvLines)
	}()
	time.Sleep(2 * time.Second) // let the receiver attach to the broadcast port

	t0 := time.Now()
	send := exec.CommandContext(ctx, tool, "send", bundle,
		"-m", fmt.Sprint(mode), "-c", fmt.Sprint(cycles), "-p", fmt.Sprint(aBcast))
	sendOut, serr := send.CombinedOutput()
	if serr != nil {
		t.Fatalf("send: %v\n%s", serr, sendOut)
	}
	// NOT the transmit time: bcast_file_tool returns as soon as it has pushed
	// the frames into Mercury's broadcast socket, and Mercury then keys them out
	// over seconds to minutes.  Logging this as "transmit" invited exactly the
	// wrong conclusion, so name it for what it is.
	enqueueElapsed := time.Since(t0)
	t.Logf("trial: frames enqueued in %s (airtime follows)", enqueueElapsed.Round(time.Millisecond))

	// --- wait for the bundle to land ---------------------------------------
	got := filepath.Join(recvDir, filepath.Base(bundle))
	// The deadline has to cover the AIRTIME, which the enqueue above says
	// nothing about.  A robust mode carrying 2 symbols per frame spends ~10
	// minutes on a bundle that QAM16C2 clears in 26 s, so a fixed few minutes
	// silently reports a transfer still in progress as a channel failure.
	waitFor := 5 * time.Minute
	if v := os.Getenv("MERCURY_BCAST_DEADLINE_S"); v != "" {
		if n, perr := strconv.Atoi(v); perr == nil {
			waitFor = time.Duration(n) * time.Second
		}
	}
	t.Logf("trial: waiting up to %s for the bundle to arrive", waitFor)
	deadline := time.Now().Add(waitFor)
	arrived := false
	for time.Now().Before(deadline) {
		if st, serr2 := os.Stat(got); serr2 == nil && st.Size() == int64(bundleSize) {
			arrived = true
			break
		}
		time.Sleep(500 * time.Millisecond)
	}
	rxElapsed := time.Since(t0)
	if !arrived {
		for {
			select {
			case l, ok := <-recvLines:
				if !ok {
					goto drained
				}
				t.Logf("recv: %s", l)
			default:
				goto drained
			}
		}
	drained:
		printLogs(t, outA.Name(), errA.Name())
		printLogs(t, outB.Name(), errB.Name())
		t.Fatalf("bundle did not arrive within the deadline (mode=%d No=%.2f fading=%q cycles=%d)",
			mode, params.No_dBHz, params.Fading, cycles)
	}
	t.Logf("trial: bundle complete off-air after %s", rxElapsed.Round(time.Millisecond))

	// --- the point: does NNCP accept what came off the air? ----------------
	rxf, oerr := os.Open(got)
	if oerr != nil {
		t.Fatal(oerr)
	}
	rxcmd := exec.Command("nncp-bundle", "-cfg", bCfg, "-quiet", "-rx")
	rxcmd.Stdin = rxf
	if out, e := rxcmd.CombinedOutput(); e != nil {
		t.Fatalf("nncp-bundle -rx rejected the off-air bundle: %v\n%s", e, out)
	}
	rxf.Close()
	if out, e := exec.Command("nncp-toss", "-cfg", bCfg, "-quiet").CombinedOutput(); e != nil {
		t.Fatalf("nncp-toss: %v\n%s", e, out)
	}
	delivered := filepath.Join(nroot, "bob", "incoming", "payload.bin")
	dgot, derr := os.ReadFile(delivered)
	if derr != nil {
		t.Fatalf("payload not delivered into bob's incoming: %v", derr)
	}
	if len(dgot) != payloadBytes {
		t.Fatalf("delivered payload is %d bytes, want %d", len(dgot), payloadBytes)
	}
	orig, _ := os.ReadFile(payload)
	for i := range orig {
		if orig[i] != dgot[i] {
			t.Fatalf("delivered payload differs at byte %d", i)
		}
	}

	goodput := float64(bundleSize*8) / rxElapsed.Seconds()
	t.Logf("TRIAL RESULT mode=%d No=%.2f fading=%q cycles=%d bundle=%dB "+
		"total=%s goodput=%.0f bps  NNCP=ACCEPTED",
		mode, params.No_dBHz, params.Fading, cycles, bundleSize,
		rxElapsed.Round(time.Millisecond), goodput)
}
