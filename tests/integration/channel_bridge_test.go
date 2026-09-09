package integration

import (
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

type ChannelParams struct {
	No_dBHz      float64
	FreqOffsetHz float64
	Gain         float64
	Fading       string // ch: "mpg"/"mpp"/"mpd"; watterson: "good"/"moderate"/"poor"/"flutter"
	Engine       string // "" or "ch" = codec2 ch.c; "watterson" = utils/watterson_test

	// Distinguishes the two directions' fading realisations.
	//
	// Both directions spawn their own channel process, and both would inherit
	// the same MERCURY_WATTERSON_SEED -- so forward and reverse would fade in
	// LOCKSTEP: one deep fade blocking the CALL and the ACCEPT at the same
	// instant, and asymmetric-link effects invisible by construction.  Real
	// paths are correlated, not identical.  Salting the seed per direction
	// keeps a pinned run exactly reproducible while making the two paths
	// independent.
	SeedSalt uint32
}

func DefaultChannelParams() ChannelParams {
	return ChannelParams{No_dBHz: -100, FreqOffsetHz: 0, Gain: 1.0}
}

func (p ChannelParams) args() []string {
	if p.Engine == "pathsim" {
		// pathsim (AE4JY PathSim port) speaks --snr, not --No, and its SNR is
		// referenced to the MEASURED signal RMS (own convention, roughly 2-3 dB
		// harsher than SNR3k at matched numbers — do not mix scales).  For this
		// engine No_dBHz carries the pathsim-native SNR in dB directly.
		a := []string{"--snr", fmt.Sprintf("%.2f", p.No_dBHz)}
		if p.Fading != "" {
			a = append(a, "--"+p.Fading)
		}
		return a
	}
	a := []string{
		"--No", fmt.Sprintf("%.2f", p.No_dBHz),
		"--freq", fmt.Sprintf("%.2f", p.FreqOffsetHz),
		"--gain", fmt.Sprintf("%.4f", p.Gain),
	}
	if p.Fading != "" {
		a = append(a, "--"+p.Fading)
	}
	return a
}

// buildWatterson builds utils/watterson_test (the fixed Watterson HF channel)
// and returns its path.  Used when MERCURY_CH_ENGINE=watterson.
func buildWatterson(repoRoot string) (string, error) {
	dst := filepath.Join(repoRoot, "utils", "watterson_test")
	cmd := exec.Command("make", "watterson_test")
	cmd.Dir = filepath.Join(repoRoot, "utils")
	out, err := cmd.CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("build watterson_test: %v\n%s", err, string(out))
	}
	if !executableExists(dst) {
		return "", fmt.Errorf("watterson_test built but not found at %s", dst)
	}
	return dst, nil
}

func buildCh(repoRoot string) (string, error) {
	dst := filepath.Join(repoRoot, "modem", "freedv", "ch")
	if executableExists(dst) {
		return dst, nil
	}
	cmd := exec.Command("gcc", "-Wall", "-O2", "-std=gnu11", "-I.", "-o", "ch", "ch.c",
		"-L.", "-lfreedvdata", "-lm")
	cmd.Dir = filepath.Join(repoRoot, "modem", "freedv")
	out, err := cmd.CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("build ch: %v\n%s", err, string(out))
	}
	if !executableExists(dst) {
		return "", fmt.Errorf("ch built but not found at %s", dst)
	}
	return dst, nil
}

type channelBridge struct {
	cancel context.CancelFunc
	wg     sync.WaitGroup
}

func startChannelBridge(ctx context.Context, chBin string,
	aTX, bRX, bTX, aRX string, params ChannelParams) *channelBridge {

	bridgeCtx, cancel := context.WithCancel(ctx)
	cb := &channelBridge{cancel: cancel}

	// Per-direction No override to reproduce an ASYMMETRIC link, e.g. the OTA
	// estacao6<->estacao10 case (workable forward, ~5 dB weaker reverse that
	// kills ACK survival).  A->B is forward (caller's data); B->A is reverse
	// (callee's ACKs).  MERCURY_CH_NO_FWD / MERCURY_CH_NO_REV (dBHz) override
	// each direction; unset = symmetric params.No_dBHz.  ch: SNR3k = -No - 14.82.
	fwd, rev := params, params
	fwd.SeedSalt, rev.SeedSalt = 1, 2
	if v := os.Getenv("MERCURY_CH_NO_FWD"); v != "" {
		if f, err := strconv.ParseFloat(v, 64); err == nil {
			fwd.No_dBHz = f
		}
	}
	if v := os.Getenv("MERCURY_CH_NO_REV"); v != "" {
		if f, err := strconv.ParseFloat(v, 64); err == nil {
			rev.No_dBHz = f
		}
	}
	// Per-direction fading profile (watterson engine: good|moderate|poor|flutter;
	// "none" disables fading on that direction).  Mimics the OTA: a workable
	// forward path and a harsher-faded reverse/ACK path.
	if v := os.Getenv("MERCURY_CH_FADING_FWD"); v != "" {
		fwd.Fading = v
		if v == "none" {
			fwd.Fading = ""
		}
	}
	if v := os.Getenv("MERCURY_CH_FADING_REV"); v != "" {
		rev.Fading = v
		if v == "none" {
			rev.Fading = ""
		}
	}

	cb.wg.Add(2)
	go func() { defer cb.wg.Done(); runChannelDir(bridgeCtx, chBin, aTX, bRX, fwd) }()
	go func() { defer cb.wg.Done(); runChannelDir(bridgeCtx, chBin, bTX, aRX, rev) }()

	return cb
}

func (cb *channelBridge) Close() {
	cb.cancel()
	cb.wg.Wait()
}

const blockSamples = 160
const blockS32 = blockSamples * 4 // 640 bytes s32le
const blockS16 = blockSamples * 2 // 320 bytes int16 s16le

func runChannelDir(ctx context.Context, chBin, txPath, rxPath string, params ChannelParams) {
	for {
		if err := runChannelDirOnce(ctx, chBin, txPath, rxPath, params); err != nil {
			return
		}
		select {
		case <-ctx.Done():
			return
		default:
		}
	}
}

func runChannelDirOnce(ctx context.Context, chBin, txPath, rxPath string, params ChannelParams) error {
	txFD, err := waitForFIFOOpen(ctx, txPath, syscall.O_RDONLY|syscall.O_NONBLOCK)
	if err != nil {
		return err
	}

	rxFD, err := waitForFIFOOpen(ctx, rxPath, syscall.O_WRONLY|syscall.O_NONBLOCK)
	if err != nil {
		syscall.Close(txFD)
		return err
	}

	// Use "-" (stdin/stdout) for BOTH engines.  Passing "/dev/stdout" made
	// watterson_test fopen() a SEPARATE fully-buffered stream, so its
	// `if (fout == stdout) fflush()` guard was false and output was never
	// flushed — it dribbled out only when the 4 KB stdio buffer filled, adding
	// ~seconds of latency that broke the real-time connect handshake.  With "-"
	// fout == stdout, the per-block fflush fires, and watterson streams like ch.
	inArg, outArg := "-", "-"
	chCmd := exec.CommandContext(ctx, chBin, inArg, outArg)
	chCmd.Args = append(chCmd.Args, params.args()...)
	chCmd.Dir = filepath.Dir(chBin) // so --fading_dir 'unittest' resolves
	chCmd.Stderr = nil
	// Give each direction its own fading realisation (see SeedSalt).  Only
	// when the run is pinned: unpinned, each channel process already seeds
	// itself from clock+pid and is independent anyway.
	if base := os.Getenv("MERCURY_WATTERSON_SEED"); base != "" && params.SeedSalt != 0 {
		if b, err := strconv.ParseUint(base, 0, 32); err == nil {
			chCmd.Env = append(os.Environ(),
				fmt.Sprintf("MERCURY_WATTERSON_SEED=%d",
					uint32(b)*2654435761+params.SeedSalt))
		}
	}

	chStdin, err := chCmd.StdinPipe()
	if err != nil {
		syscall.Close(txFD)
		syscall.Close(rxFD)
		return err
	}
	chStdout, err := chCmd.StdoutPipe()
	if err != nil {
		chStdin.Close()
		syscall.Close(txFD)
		syscall.Close(rxFD)
		return err
	}
	if err := chCmd.Start(); err != nil {
		chStdin.Close()
		chStdout.Close()
		syscall.Close(txFD)
		syscall.Close(rxFD)
		return err
	}

	done := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
			chStdin.Close()
			chStdout.Close()
			syscall.Close(txFD)
			syscall.Close(rxFD)
		case <-done:
		}
	}()

	// Two independent streaming pumps instead of a write/read lockstep:
	// ch consumes and produces 160-sample blocks but its internal pipe
	// buffering must not be coupled to our scheduling.  The TX pump reads
	// whatever Mercury wrote (a whole burst arrives much faster than real
	// time), converts s32le→s16le and streams it into ch; the RX pump
	// streams ch output back as s32le into the peer's capture FIFO.

	var inBytes, outBytes, droppedBytes int64
	pumpDone := make(chan error, 2)

	// Diagnosis: is the bridge itself falling behind real time?  inBytes is
	// s32 read off the sender's TX FIFO, outBytes s32 written to the peer's
	// capture FIFO; both are 4 bytes per 8 kHz sample, so their difference is
	// audio that entered the channel and has not been delivered yet.
	if os.Getenv("MERCURY_CH_DIAG") != "" {
		t0 := time.Now()
		go func() {
			tk := time.NewTicker(time.Second)
			defer tk.Stop()
			for {
				select {
				case <-ctx.Done():
					return
				case <-tk.C:
					in := atomic.LoadInt64(&inBytes)
					out := atomic.LoadInt64(&outBytes)
					fmt.Printf("CHDIAG %-14s %7.3f keyed=%.2fs delivered=%.2fs dropped=%.2fs\n",
						filepath.Base(txPath), time.Since(t0).Seconds(),
						float64(in)/4/8000, float64(out)/4/8000,
						float64(atomic.LoadInt64(&droppedBytes))/4/8000)
				}
			}
		}()
	}

	go func() { // TX FIFO -> ch stdin, paced at real-time 8 kHz
		// A sound card consumes 8 kHz continuously whether or not the
		// operator is transmitting, so the channel -- and the receiver
		// behind it -- sees an unbroken sample stream.  Feed ch the same
		// way: one 160-sample block every 20 ms, taken from Mercury's TX
		// FIFO when it has audio and silence when it does not.
		//
		// Handing ch only the bursts, as this pump used to, leaves the
		// peer's demodulator holding the tail of each burst: it needs a
		// whole nin() block to run, the burst ends part-way through one,
		// and no further audio arrives to complete it until the next
		// transmission -- a full retry period later.  That is not a
		// throughput detail, it inverts the connect handshake.  Measured
		// on this harness at seed 6: the answerer had every sample of the
		// caller's CALL by t=4.0 s, decoded it at t=12.2 s (when the
		// caller's *second* CALL began arriving), and keyed its ACCEPT at
		// t=12.9 s -- inside the caller's 11.7-15.4 s transmission, where
		// a half-duplex radio is deaf. Every round, so the connect never
		// completed and the failure looked like a modem bug.
		//
		// Feeding silence also advances the fading process in wall-clock
		// time instead of only while somebody transmits, which is what
		// the Watterson model assumes.
		buf := make([]byte, 64*1024)
		s16 := make([]byte, blockS16)
		var pending []byte // s16le drained from the FIFO, not yet fed to ch
		carry := 0
		deadline := time.Now()

		for {
			select {
			case <-ctx.Done():
				pumpDone <- ctx.Err()
				return
			default:
			}

			// Drain whatever Mercury has written; a whole burst lands
			// far faster than real time and queues up here.
			for {
				n, err := syscall.Read(txFD, buf[carry:])
				if n > 0 {
					n += carry
					whole := n &^ 3 // s32le sample alignment
					for i := 0; i < whole/4; i++ {
						v := int32(binary.LittleEndian.Uint32(buf[i*4 : i*4+4]))
						var b [2]byte
						binary.LittleEndian.PutUint16(b[:], uint16(int16(v>>16)))
						pending = append(pending, b[0], b[1])
					}
					atomic.AddInt64(&inBytes, int64(whole))
					carry = n - whole
					copy(buf[:carry], buf[whole:n])
					continue
				}
				if err == syscall.EAGAIN || (err == nil && n == 0) {
					break // nothing queued right now
				}
				pumpDone <- err
				return
			}

			// One block per 20 ms on an absolute clock.
			now := time.Now()
			if deadline.Before(now) {
				deadline = now
			}
			time.Sleep(deadline.Sub(now))
			deadline = deadline.Add(20 * time.Millisecond)

			if len(pending) >= blockS16 {
				copy(s16, pending[:blockS16])
				pending = pending[blockS16:]
				if len(pending) == 0 {
					pending = nil
				}
			} else {
				// Idle (or a burst that ends mid-block): silence.
				n := copy(s16, pending)
				for i := n; i < blockS16; i++ {
					s16[i] = 0
				}
				pending = nil
			}

			if _, werr := chStdin.Write(s16); werr != nil {
				pumpDone <- werr
				return
			}
		}
	}()

	go func() { // ch stdout -> RX FIFO
		// The TX pump above already meters audio into ch at 8 kHz, so ch
		// emits at real time and this side only has to forward.  The
		// absolute-clock sleep is kept as a backstop against ch emitting a
		// burst of blocks after a scheduling hiccup: without it the peer
		// could decode and reply while the sender still holds PTT, and the
		// half-duplex RX path would discard the reply.
		s16 := make([]byte, blockS16)
		s32 := make([]byte, blockS32)
		deadline := time.Now()
		for {
			n, err := io.ReadFull(chStdout, s16)
			if n > 0 {
				whole := n &^ 1
				for i := 0; i < whole/2; i++ {
					v := int16(binary.LittleEndian.Uint16(s16[i*2 : i*2+2]))
					binary.LittleEndian.PutUint32(s32[i*4:i*4+4], uint32(int32(v)<<16))
				}
				// Absolute-clock pacing: one 160-sample block per 20 ms.
				now := time.Now()
				if deadline.Before(now) {
					deadline = now // idle gap: restart the pacing clock
				}
				time.Sleep(deadline.Sub(now))
				deadline = deadline.Add(20 * time.Millisecond)

				// Deliver, or DROP -- never queue.  A receiver that is not
				// listening (half-duplex: it is transmitting) loses the audio
				// on a real link; there is no buffer in the air holding it
				// back for later.  Retrying here until the peer drains turns
				// the FIFO into exactly such a buffer: the peer stops reading
				// while it transmits, this pump stalls, and the backlog is
				// handed over AFTER its PTT drops -- past the flush that was
				// meant to discard it.  The peer then decodes a burst that
				// left the air seconds earlier and answers into a window that
				// has already closed.  This is a fidelity principle, not a
				// measured regression: a radio has no such buffer.
				//
				// So write what the kernel takes and discard the rest, which
				// is what the radio does.
				written := 0
				for written < whole*2 {
					wn, werr := syscall.Write(rxFD, s32[written:whole*2])
					if wn > 0 {
						written += wn
						continue
					}
					if werr == syscall.EAGAIN {
						atomic.AddInt64(&droppedBytes, int64(whole*2-written))
						break
					}
					if werr != nil {
						pumpDone <- werr
						return
					}
				}
				atomic.AddInt64(&outBytes, int64(written))
			}
			if err != nil {
				pumpDone <- err
				return
			}
		}
	}()

	err = <-pumpDone
	fmt.Printf("channel bridge %s->%s: %d bytes in, %d bytes out (%v)\n",
		filepath.Base(txPath), filepath.Base(rxPath),
		atomic.LoadInt64(&inBytes), atomic.LoadInt64(&outBytes), err)

	close(done)
	chStdin.Close()
	chStdout.Close()
	chCmd.Process.Kill()
	chCmd.Wait()
	syscall.Close(txFD)
	syscall.Close(rxFD)

	select {
	case <-ctx.Done():
		return ctx.Err()
	default:
		return nil
	}
}

func s32toS16block(s32, s16 []byte) {
	for i := 0; i < blockSamples; i++ {
		v := int32(binary.LittleEndian.Uint32(s32[i*4 : i*4+4]))
		binary.LittleEndian.PutUint16(s16[i*2:i*2+2], uint16(int16(v>>16)))
	}
}

func s16toS32block(s16, s32 []byte) {
	for i := 0; i < blockSamples; i++ {
		v := int16(binary.LittleEndian.Uint16(s16[i*2 : i*2+2]))
		binary.LittleEndian.PutUint32(s32[i*4:i*4+4], uint32(int32(v)<<16))
	}
}
