package integration

// Deterministic two-station bench over the -x sock lockstep transport.
//
// The FIFO bridge paces audio in wall-clock time, so a burst lands in a
// slightly different part of the fading process on every run: the same seed
// does NOT reproduce a run.  At the fringe, where a decode is marginal
// anyway, tens of milliseconds of scheduling jitter flips the outcome, and a
// pass/fail measured that way carries no information about a code change.
//
// Under -x sock the simulator owns time.  Each station blocks until the sim
// hands it a block, deposits it, advances its virtual clock to the timestamp
// in that block, waits for its own demodulator to drain, and only then
// replies with the audio it transmitted.  Nothing advances unless the sim
// says so, so a run is a pure function of the seed and the binaries.
//
// How far the determinism goes, measured rather than assumed:
//
//   - A run that never leaves the connect handshake reproduces EXACTLY.  Four
//     runs of one seed gave byte-identical keying logs and the same 2934
//     blocks.  On the FIFO bridge the same seed gave 2995 / 3811 / 3523.
//   - Once the data plane is running, two runs drift by about one block
//     (20 ms) and then accumulate: 5636 / 5641 / 5643 blocks for one seed,
//     identical up to the end of the handshake.
//
// The residual drift is not in this file.  arq_notify_virtual_time() only
// SIGNALS the ARQ event loop after the clock advances; it does not wait for
// it, so whether the loop has fired a due deadline before the station replies
// is still a thread race.  Closing it would mean having the station report
// that it has processed a timestamp and having the transport block on that --
// a change inside mercury for the benefit of a test, which is a trade worth
// making deliberately or not at all.
//
// So: trust this bench for connect-path work, and treat data-phase timings as
// reproducible to about a block.
//
// Wire format: audioio/sock_wire.h (little-endian, pinned by
// tests/audioio/test_sock_wire.c).
//   sim -> station:  u32 len | u64 seq | u64 virtual_now_ms | u16 n | n i16
//   station -> sim:  u32 len | u64 seq | u8 ptt | u16 n | n i16

import (
	"bytes"
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"math"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"sync"
	"time"
)

const (
	sockBlockSamples = 160 // 20 ms at 8 kHz
	sockBlockMs      = 20
	sockHdrSim       = 18
	sockHdrSta       = 11
)

type sockStation struct {
	name string
	conn net.Conn
	rd   []byte
	wr   []byte
}

type sockSim struct {
	lnA, lnB net.Listener
	a, b     *sockStation
	fwd, rev *sockChannel
	wg       sync.WaitGroup
	cancel   context.CancelFunc
	keyedA   int64
	keyedB   int64
	blocks   uint64
	prevA    uint8
	prevB    uint8
	late     int64 // blocks delivered behind their cadence deadline
	maxLate  int64 // worst lateness, milliseconds
	rmsA     float64
	rmsB     float64
	txPwrA   float64
	txPwrB   float64
	txBlkA   int64
	txBlkB   int64
	// Level delivered to each station while the OTHER one is keyed, i.e. the
	// level of the burst it is supposed to decode, separated from the idle
	// noise floor it hears the rest of the time.
	sigA, sigB   float64
	sigNA, sigNB int64
	nfA, nfB     float64
	nfNA, nfNB   int64
	// keying is the PTT transition log indexed by block, i.e. purely in
	// sample time.  It is measured OUTSIDE mercury, so it cannot be fooled
	// by internal thread scheduling, and it is what two runs of the same
	// seed are compared on.
	keying []string
}

// sockChannel is one direction through the external channel model, driven
// strictly block-for-block so its state advances only with virtual time.
type sockChannel struct {
	cmd    *exec.Cmd
	stdin  io.WriteCloser
	stdout io.ReadCloser
	in     []byte
	out    []byte
	errBuf *bytes.Buffer
}

// MeasuredSNR3k returns the channel model's own SNR3k reading, and the duty
// correction that converts it to an in-burst figure: the model averages signal
// power over every sample it processed, including the silence between bursts,
// so a link that transmits d of the time reads 10log10(d) dB low.
func (c *sockChannel) MeasuredSNR3k(duty float64) (reported, inBurst float64, ok bool) {
	m := regexp.MustCompile(`SNR3k\(dB\):\s*([-\d.]+)`).FindStringSubmatch(c.errBuf.String())
	if m == nil {
		return 0, 0, false
	}
	v, err := strconv.ParseFloat(m[1], 64)
	if err != nil || duty <= 0 {
		return 0, 0, false
	}
	return v, v - 10*math.Log10(duty), true
}

func startSockChannel(ctx context.Context, chBin string, p ChannelParams) (*sockChannel, error) {
	// NOT CommandContext: cancelling the context would kill the model before
	// it can print its measured SNR on exit.  Close() drains it instead.
	cmd := exec.Command(chBin, "-", "-")
	cmd.Args = append(cmd.Args, p.args()...)
	cmd.Dir = filepath.Dir(chBin)
	// The channel model measures its own signal/noise ratio and prints it on
	// exit.  Keep it: the bench's SNR axis must be MEASURED, not derived from
	// a No->SNR formula carried over from another tool.  Three different
	// formulas for this bench disagreed by ~12 dB, which is enough to make
	// any comparison against a standalone modem test meaningless.
	var errBuf bytes.Buffer
	cmd.Stderr = &errBuf
	if base := os.Getenv("MERCURY_WATTERSON_SEED"); base != "" && p.SeedSalt != 0 {
		if v, err := parseSeed(base); err == nil {
			cmd.Env = append(os.Environ(),
				fmt.Sprintf("MERCURY_WATTERSON_SEED=%d", v*2654435761+p.SeedSalt))
		}
	}
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, err
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	return &sockChannel{
		cmd: cmd, stdin: stdin, stdout: stdout, errBuf: &errBuf,
		in:  make([]byte, sockBlockSamples*2),
		out: make([]byte, sockBlockSamples*2),
	}, nil
}

// process pushes one block through the channel and returns the block that
// comes out.  1:1 in samples, so the model's delay line and fading state
// advance exactly one block per call.
func (c *sockChannel) process(samples []int16) ([]int16, error) {
	for i, s := range samples {
		binary.LittleEndian.PutUint16(c.in[i*2:], uint16(s))
	}
	if _, err := c.stdin.Write(c.in); err != nil {
		return nil, err
	}
	if _, err := io.ReadFull(c.stdout, c.out); err != nil {
		return nil, err
	}
	out := make([]int16, sockBlockSamples)
	for i := range out {
		out[i] = int16(binary.LittleEndian.Uint16(c.out[i*2:]))
	}
	return out, nil
}

func (c *sockChannel) Close() {
	// Close stdin and let the model drain: it prints its measured SNR3k on
	// exit, and killing it discards exactly the calibration the bench needs.
	c.stdin.Close()
	done := make(chan struct{})
	go func() { c.cmd.Wait(); close(done) }()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		if c.cmd.Process != nil {
			c.cmd.Process.Kill()
		}
		<-done
	}
	c.stdout.Close()
}

func parseSeed(s string) (uint32, error) {
	var v uint64
	_, err := fmt.Sscan(s, &v)
	return uint32(v), err
}

// sendSim writes one sim->station frame.
func (st *sockStation) sendSim(seq, vnowMs uint64, samples []int16) error {
	n := uint16(len(samples))
	total := sockHdrSim + int(n)*2
	if cap(st.wr) < 4+total {
		st.wr = make([]byte, 4+total)
	}
	buf := st.wr[:4+total]
	binary.LittleEndian.PutUint32(buf, uint32(total))
	binary.LittleEndian.PutUint64(buf[4:], seq)
	binary.LittleEndian.PutUint64(buf[12:], vnowMs)
	binary.LittleEndian.PutUint16(buf[20:], n)
	for i, s := range samples {
		binary.LittleEndian.PutUint16(buf[22+i*2:], uint16(s))
	}
	_, err := st.conn.Write(buf)
	return err
}

// recvStation reads one station->sim frame: the audio it transmitted this
// block, and whether its PTT was keyed.
func (st *sockStation) recvStation() (ptt uint8, samples []int16, err error) {
	var lenbuf [4]byte
	if _, err = io.ReadFull(st.conn, lenbuf[:]); err != nil {
		return
	}
	total := int(binary.LittleEndian.Uint32(lenbuf[:]))
	if total < sockHdrSta {
		return 0, nil, fmt.Errorf("short station frame: %d", total)
	}
	if cap(st.rd) < total {
		st.rd = make([]byte, total)
	}
	body := st.rd[:total]
	if _, err = io.ReadFull(st.conn, body); err != nil {
		return
	}
	ptt = body[8]
	n := int(binary.LittleEndian.Uint16(body[9:]))
	if sockHdrSta+n*2 != total {
		return 0, nil, fmt.Errorf("station frame length %d disagrees with n=%d", total, n)
	}
	samples = make([]int16, n)
	for i := range samples {
		samples[i] = int16(binary.LittleEndian.Uint16(body[sockHdrSta+i*2:]))
	}
	return ptt, samples, nil
}

// startSockSim listens on the two station sockets, waits for both stations to
// connect, and drives the exchange until ctx is cancelled.
func startSockSim(ctx context.Context, chBin, pathA, pathB string,
	fwd, rev ChannelParams) (*sockSim, error) {

	lnA, err := net.Listen("unix", pathA)
	if err != nil {
		return nil, err
	}
	lnB, err := net.Listen("unix", pathB)
	if err != nil {
		lnA.Close()
		return nil, err
	}
	simCtx, cancel := context.WithCancel(ctx)
	s := &sockSim{lnA: lnA, lnB: lnB, cancel: cancel}

	fwdCh, err := startSockChannel(simCtx, chBin, fwd)
	if err != nil {
		cancel()
		lnA.Close()
		lnB.Close()
		return nil, err
	}
	revCh, err := startSockChannel(simCtx, chBin, rev)
	if err != nil {
		cancel()
		fwdCh.Close()
		lnA.Close()
		lnB.Close()
		return nil, err
	}
	s.fwd, s.rev = fwdCh, revCh

	// Accept both stations before stepping time.
	type acc struct {
		c   net.Conn
		err error
	}
	ca, cb := make(chan acc, 1), make(chan acc, 1)
	go func() { c, e := lnA.Accept(); ca <- acc{c, e} }()
	go func() { c, e := lnB.Accept(); cb <- acc{c, e} }()

	s.wg.Add(1)
	go func() {
		defer s.wg.Done()
		ra, rb := <-ca, <-cb
		if ra.err != nil || rb.err != nil {
			return
		}
		s.a = &sockStation{name: "A", conn: ra.c}
		s.b = &sockStation{name: "B", conn: rb.c}
		defer ra.c.Close()
		defer rb.c.Close()
		s.run(simCtx)
	}()
	return s, nil
}

// run is the lockstep loop.  One block of virtual time per iteration: hand
// each station what it hears, collect what it transmitted, push that through
// the channel for the next block.  Nothing here reads a wall clock.
func (s *sockSim) run(ctx context.Context) {
	silence := make([]int16, sockBlockSamples)
	rxA, rxB := silence, silence
	var seq, vnow uint64

	// Proper cadence.  The virtual timestamp on every block is derived from
	// the SAMPLE COUNT, so the stations' ARQ clocks advance in exact step
	// with the audio -- no drift, no jitter, and a deadline can never be
	// skewed by host load the way it is when the clock is read off the wall.
	//
	// The blocks are then released against an ABSOLUTE schedule (start plus
	// k periods, never "sleep 20 ms"), so a slow iteration is absorbed
	// instead of accumulating into a drifting sample rate.  That is the
	// property the FIFO bridge never had: it paced each write relative to
	// the last one, so every scheduling hiccup permanently displaced the
	// audio, and a burst landed in a different part of the fade on every
	// run.
	var prevPttA, prevPttB uint8
	start := time.Now()
	// MERCURY_TEST_SOCK_FAST=1 releases blocks as fast as the stations accept
	// them.  Determinism comes from the sample clock and the lockstep, not
	// from the pacing, so a sweep can run far quicker than real time -- but
	// the DSP threads then run against a compressed timeline, so keep the
	// paced default for anything that models a radio.
	fast := os.Getenv("MERCURY_TEST_SOCK_FAST") == "1"

	for {
		select {
		case <-ctx.Done():
			return
		default:
		}

		// Release this block on schedule.  Lateness is recorded rather than
		// silently absorbed: a bench that has fallen behind real time is no
		// longer modelling a radio, and the test should be able to say so.
		deadline := start.Add(time.Duration(seq) * sockBlockMs * time.Millisecond)
		if d := time.Until(deadline); fast {
			// no pacing
		} else if d > 0 {
			select {
			case <-ctx.Done():
				return
			case <-time.After(d):
			}
		} else if lateMs := (-d).Milliseconds(); lateMs > sockBlockMs {
			s.late++
			if lateMs > s.maxLate {
				s.maxLate = lateMs
			}
		}

		if err := s.a.sendSim(seq, vnow, rxA); err != nil {
			s.stop("send A", err)
			return
		}
		if err := s.b.sendSim(seq, vnow, rxB); err != nil {
			s.stop("send B", err)
			return
		}

		pttA, txA, err := s.a.recvStation()
		if err != nil {
			s.stop("recv A", err)
			return
		}
		pttB, txB, err := s.b.recvStation()
		if err != nil {
			s.stop("recv B", err)
			return
		}
		if pttA == 1 {
			s.keyedA++
		}
		if pttB == 1 {
			s.keyedB++
		}
		// Printed as they happen, not buffered to the end: a run killed by a
		// test timeout still has to yield its keying history, and a deferred
		// dump is exactly what a panic skips.
		if pttA != s.prevA {
			line := fmt.Sprintf("A %-5s @%6.2fs", pttName(pttA), float64(vnow)/1000)
			s.keying = append(s.keying, line)
			fmt.Printf("SOCKSIM keying: %s\n", line)
			s.prevA = pttA
		}
		if pttB != s.prevB {
			line := fmt.Sprintf("B %-5s @%6.2fs", pttName(pttB), float64(vnow)/1000)
			s.keying = append(s.keying, line)
			fmt.Printf("SOCKSIM keying: %s\n", line)
			s.prevB = pttB
		}
		if pttA != s.prevA {
			s.keying = append(s.keying, fmt.Sprintf("A %s @block %d", pttName(pttA), s.blocks))
			s.prevA = pttA
		}
		if pttB != s.prevB {
			s.keying = append(s.keying, fmt.Sprintf("B %s @block %d", pttName(pttB), s.blocks))
			s.prevB = pttB
		}

		// Half-duplex: a station hears nothing while its own PTT is keyed.
		// The channel still runs on silence so its fading state advances with
		// virtual time rather than only while someone transmits.
		if rxB, err = s.fwd.process(txA); err != nil {
			s.stop("fwd channel", err)
			return
		}
		if rxA, err = s.rev.process(txB); err != nil {
			s.stop("rev channel", err)
			return
		}
		if pttB == 1 {
			rxB = silence
		}
		if pttA == 1 {
			rxA = silence
		}

		// Track what each station is actually being handed.  A receiver that
		// hears digital silence looks identical to one that hears a dead
		// channel, and the two have very different causes.
		s.rmsA += blockPower(rxA)
		s.rmsB += blockPower(rxB)
		// rxA carries what B transmitted one block ago, and vice versa.
		if prevPttB == 1 {
			s.sigA += blockPower(rxA)
			s.sigNA++
		} else {
			s.nfA += blockPower(rxA)
			s.nfNA++
		}
		if prevPttA == 1 {
			s.sigB += blockPower(rxB)
			s.sigNB++
		} else {
			s.nfB += blockPower(rxB)
			s.nfNB++
		}
		prevPttA, prevPttB = pttA, pttB
		// Level of what each station puts ON AIR while keyed.  A keyed
		// transmitter emitting silence is a very different fault from a
		// receiver that cannot acquire.
		if pttA == 1 {
			s.txPwrA += blockPower(txA)
			s.txBlkA++
		}
		if pttB == 1 {
			s.txPwrB += blockPower(txB)
			s.txBlkB++
		}

		seq++
		vnow += sockBlockMs
		s.blocks++
	}
}

// stop records why the lockstep loop ended.  A sim that dies early looks
// exactly like a link that went quiet, so it must never fail silently.
func (s *sockSim) stop(where string, err error) {
	fmt.Printf("SOCKSIM stopped at block %d (%s): %v\n", s.blocks, where, err)
}

func blockPower(b []int16) float64 {
	var acc float64
	for _, v := range b {
		acc += float64(v) * float64(v)
	}
	return acc / float64(len(b))
}

// ChannelSNR reports each direction's MEASURED SNR3k, corrected to an in-burst
// figure using the transmit duty cycle the sim actually observed.
func (s *sockSim) ChannelSNR() string {
	out := ""
	for _, d := range []struct {
		name         string
		ch           *sockChannel
		keyed, total int64
	}{
		{"A->B", s.fwd, s.txBlkA, int64(s.blocks)},
		{"B->A", s.rev, s.txBlkB, int64(s.blocks)},
	} {
		if d.total == 0 {
			continue
		}
		duty := float64(d.keyed) / float64(d.total)
		rep, burst, ok := d.ch.MeasuredSNR3k(duty)
		if !ok {
			out += fmt.Sprintf("%s=n/a ", d.name)
			continue
		}
		out += fmt.Sprintf("%s: reported %.1f dB, duty %.2f -> in-burst %.1f dB   ", d.name, rep, duty, burst)
	}
	return out
}

// BurstLevels reports, per station, the level it was handed while the peer was
// keyed and the level while the peer was silent -- signal versus noise floor.
func (s *sockSim) BurstLevels() (aSig, aNf, bSig, bNf float64) {
	d := func(p float64, n int64) float64 {
		if n == 0 || p <= 0 {
			return -999
		}
		return 10 * math.Log10((p/float64(n))/(32768.0*32768.0))
	}
	return d(s.sigA, s.sigNA), d(s.nfA, s.nfNA), d(s.sigB, s.sigNB), d(s.nfB, s.nfNB)
}

// TxLevels reports the mean on-air level of each station while keyed, dBFS.
func (s *sockSim) TxLevels() (a, b float64) {
	dbfs := func(p float64, n int64) float64 {
		if n == 0 || p <= 0 {
			return -999
		}
		return 10 * math.Log10((p/float64(n))/(32768.0*32768.0))
	}
	return dbfs(s.txPwrA, s.txBlkA), dbfs(s.txPwrB, s.txBlkB)
}

// RxLevels reports the mean received level handed to each station, as dBFS.
func (s *sockSim) RxLevels() (a, b float64) {
	n := float64(s.blocks)
	if n == 0 {
		return -999, -999
	}
	dbfs := func(p float64) float64 {
		if p <= 0 {
			return -999
		}
		return 10 * math.Log10(p/(32768.0*32768.0))
	}
	return dbfs(s.rmsA / n), dbfs(s.rmsB / n)
}

func pttName(p uint8) string {
	if p == 1 {
		return "key"
	}
	return "unkey"
}

// KeyingLog is the PTT transition sequence in sample time.  Two runs of the
// same seed must produce the same list; it is the bench's determinism check.
func (s *sockSim) KeyingLog() []string { return s.keying }

// Cadence reports how well the bench held its schedule.
func (s *sockSim) Cadence() (blocks uint64, late int64, maxLateMs int64) {
	return s.blocks, s.late, s.maxLate
}

func (s *sockSim) Close() {
	// Drain the channel models BEFORE cancelling: they print their measured
	// SNR when their input ends, and that is the bench's calibration.
	s.fwd.Close()
	s.rev.Close()
	s.cancel()
	s.lnA.Close()
	s.lnB.Close()
	s.wg.Wait()
}
