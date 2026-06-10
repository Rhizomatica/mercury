package integration

import (
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"os/exec"
	"path/filepath"
	"sync"
	"syscall"
	"time"
)

type ChannelParams struct {
	No_dBHz      float64
	FreqOffsetHz float64
	Gain         float64
}

func DefaultChannelParams() ChannelParams {
	return ChannelParams{No_dBHz: -100, FreqOffsetHz: 0, Gain: 1.0}
}

func (p ChannelParams) args() []string {
	return []string{
		"--No", fmt.Sprintf("%.2f", p.No_dBHz),
		"--freq", fmt.Sprintf("%.2f", p.FreqOffsetHz),
		"--gain", fmt.Sprintf("%.4f", p.Gain),
	}
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

	cb.wg.Add(2)
	go func() { defer cb.wg.Done(); runChannelDir(bridgeCtx, chBin, aTX, bRX, params) }()
	go func() { defer cb.wg.Done(); runChannelDir(bridgeCtx, chBin, bTX, aRX, params) }()

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

	chCmd := exec.CommandContext(ctx, chBin, "-", "-")
	chCmd.Args = append(chCmd.Args, params.args()...)
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

	var buf [blockS32 + 4096]byte
	bufLen := 0
	s16block := make([]byte, blockS16)
	ticker := time.NewTicker(19 * time.Millisecond)
	defer ticker.Stop()

loop:
	for {
		select {
		case <-ctx.Done():
			break loop
		case <-ticker.C:
		}

		// Read available bytes from TX FIFO (non-blocking)
		for {
			var tmp [4096]byte
			n, err := syscall.Read(txFD, tmp[:])
			if n > 0 {
				if bufLen+n <= len(buf) {
					copy(buf[bufLen:], tmp[:n])
					bufLen += n
				}
			}
			if err != nil {
				break
			}
		}

		// Process blocks when we have enough data
		for bufLen >= blockS32 {
			s32toS16block(buf[:blockS32], s16block)

			if _, err := chStdin.Write(s16block); err != nil {
				break loop
			}

			s16out := make([]byte, blockS16)
			if _, err := io.ReadFull(chStdout, s16out); err != nil {
				break loop
			}

			s32out := make([]byte, blockS32)
			s16toS32block(s16out, s32out)

			written := 0
			for written < len(s32out) {
				select {
				case <-ctx.Done():
					break loop
				default:
				}
				wn, werr := syscall.Write(rxFD, s32out[written:])
				if werr != nil {
					break loop
				}
				written += wn
			}

			copy(buf[:], buf[blockS32:bufLen])
			bufLen -= blockS32
		}
	}

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
