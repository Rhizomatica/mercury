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
	"syscall"
	"time"
)

type ChannelParams struct {
	No_dBHz      float64
	FreqOffsetHz float64
	Gain         float64
	Fading       string // ch: "mpg"/"mpp"/"mpd"; watterson: "good"/"moderate"/"poor"/"flutter"
	Engine       string // "" or "ch" = codec2 ch.c; "watterson" = utils/watterson_test
}

func DefaultChannelParams() ChannelParams {
	return ChannelParams{No_dBHz: -100, FreqOffsetHz: 0, Gain: 1.0}
}

func (p ChannelParams) args() []string {
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

	var inBytes, outBytes int64
	pumpDone := make(chan error, 2)

	go func() { // TX FIFO -> ch stdin
		buf := make([]byte, 64*1024)
		s16 := make([]byte, 32*1024)
		carry := 0
		for {
			select {
			case <-ctx.Done():
				pumpDone <- ctx.Err()
				return
			default:
			}
			n, err := syscall.Read(txFD, buf[carry:])
			if n > 0 {
				n += carry
				whole := n &^ 3 // s32le sample alignment
				for i := 0; i < whole/4; i++ {
					v := int32(binary.LittleEndian.Uint32(buf[i*4 : i*4+4]))
					binary.LittleEndian.PutUint16(s16[i*2:i*2+2], uint16(int16(v>>16)))
				}
				if _, werr := chStdin.Write(s16[:whole/2]); werr != nil {
					pumpDone <- werr
					return
				}
				inBytes += int64(whole)
				carry = n - whole
				copy(buf[:carry], buf[whole:n])
				continue
			}
			if err == syscall.EAGAIN || (err == nil && n == 0) {
				time.Sleep(2 * time.Millisecond) // idle: no TX in progress
				continue
			}
			pumpDone <- err
			return
		}
	}()

	go func() { // ch stdout -> RX FIFO, paced at real-time 8 kHz
		// Mercury's TX side writes a whole burst into its FIFO immediately
		// while holding PTT for the frame's nominal wall-clock duration.
		// Delivery to the peer must be paced at the sample rate: otherwise
		// the peer decodes and replies while the sender still has PTT on,
		// and the half-duplex RX path discards the reply (TX drain/flush).
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

				written := 0
				for written < whole*2 {
					wn, werr := syscall.Write(rxFD, s32[written:whole*2])
					if wn > 0 {
						written += wn
					}
					if werr != nil {
						if werr == syscall.EAGAIN {
							// Peer capture FIFO momentarily full.
							time.Sleep(2 * time.Millisecond)
							continue
						}
						pumpDone <- werr
						return
					}
				}
				outBytes += int64(whole * 2)
			}
			if err != nil {
				pumpDone <- err
				return
			}
		}
	}()

	err = <-pumpDone
	fmt.Printf("channel bridge %s->%s: %d bytes in, %d bytes out (%v)\n",
		filepath.Base(txPath), filepath.Base(rxPath), inBytes, outBytes, err)

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
