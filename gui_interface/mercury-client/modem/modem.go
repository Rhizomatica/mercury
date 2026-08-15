package modem

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"net"
	"os"
	"strings"
	"sync"
	"time"
)

const (
	FEND  byte = 0xC0
	FESC  byte = 0xDB
	TFEND byte = 0xDC
	TFESC byte = 0xDD
)

func KISSEncode(data []byte) []byte {
	var buffer bytes.Buffer
	buffer.WriteByte(FEND)
	for _, b := range data {
		switch b {
		case FEND:
			buffer.WriteByte(FESC)
			buffer.WriteByte(TFEND)
		case FESC:
			buffer.WriteByte(FESC)
			buffer.WriteByte(TFESC)
		default:
			buffer.WriteByte(b)
		}
	}
	buffer.WriteByte(FEND)
	return buffer.Bytes()
}

func KISSDecode(frame []byte) ([]byte, error) {
	var buffer bytes.Buffer
	for i := 0; i < len(frame); i++ {
		b := frame[i]
		if b == FESC {
			i++
			if i >= len(frame) {
				return nil, fmt.Errorf("malformed KISS frame: unexpected end after FESC")
			}
			switch frame[i] {
			case TFEND:
				buffer.WriteByte(FEND)
			case TFESC:
				buffer.WriteByte(FESC)
			default:
				return nil, fmt.Errorf("malformed KISS frame: unknown escape sequence %02X %02X", b, frame[i])
			}
		} else {
			buffer.WriteByte(b)
		}
	}
	return buffer.Bytes(), nil
}

// TrimKISSCommand strips the KISS command byte (the first byte of a decoded
// frame) so the returned payload is the raw message data. The Mercury broadcast
// reply frames are written as FEND <cmd> <payload> FEND, so the command byte
// must be removed before treating the frame as a message.
func TrimKISSCommand(decoded []byte) []byte {
	if len(decoded) > 0 {
		return decoded[1:]
	}
	return decoded
}

type ModemClient struct {
	ARQControlConn *net.TCPConn
	ARQDataConn    *net.TCPConn
	BroadcastConn  *net.TCPConn

	ARQControlAddr string
	ARQDataAddr    string
	BroadcastAddr  string

	LogCh             chan string
	IncomingARQCh     chan string
	IncomingARQDataCh chan []byte
	IncomingBcastCh   chan []byte
	StatusCh          chan string

	arqConnected bool

	connectRespCh chan string

	mu   sync.Mutex
	quit chan struct{}
}

func NewModemClient(arqControlAddr, arqDataAddr, broadcastAddr string) *ModemClient {
	return &ModemClient{
		ARQControlAddr:    arqControlAddr,
		ARQDataAddr:       arqDataAddr,
		BroadcastAddr:     broadcastAddr,
		LogCh:             make(chan string, 100),
		IncomingARQCh:     make(chan string, 100),
		IncomingARQDataCh: make(chan []byte, 100),
		IncomingBcastCh:   make(chan []byte, 100),
		StatusCh:          make(chan string, 100),
		quit:              make(chan struct{}),
	}
}

func (mc *ModemClient) Connect() (err error) {
	mc.mu.Lock()

	var logLines []string

	// Runs last: release the mutex, then flush the collected log lines.
	defer func() {
		quit := mc.quit
		mc.mu.Unlock()
		for _, line := range logLines {
			select {
			case mc.LogCh <- line:
			case <-quit:
				return
			}
		}
	}()

	if mc.ARQControlConn != nil || mc.ARQDataConn != nil || mc.BroadcastConn != nil {
		return fmt.Errorf("already connected")
	}

	// Runs first: on failure, close any conns opened up to this point.
	defer func() {
		if err == nil {
			return
		}
		control := mc.ARQControlConn
		if control != nil {
			control.Close()
			mc.ARQControlConn = nil
		}
		if mc.ARQDataConn != nil && mc.ARQDataConn != control {
			mc.ARQDataConn.Close()
			mc.ARQDataConn = nil
		}
		if mc.BroadcastConn != nil {
			mc.BroadcastConn.Close()
			mc.BroadcastConn = nil
		}
	}()

	arqControlTCPAddr, err := net.ResolveTCPAddr("tcp", mc.ARQControlAddr)
	if err != nil {
		return fmt.Errorf("resolve ARQ control address: %w", err)
	}
	mc.ARQControlConn, err = net.DialTCP("tcp", nil, arqControlTCPAddr)
	if err != nil {
		return fmt.Errorf("connect to ARQ control port %s: %w", mc.ARQControlAddr, err)
	}
	logLines = append(logLines, fmt.Sprintf("Connected to ARQ Control: %s", mc.ARQControlAddr))

	if mc.ARQDataAddr != "" && mc.ARQDataAddr != mc.ARQControlAddr {
		arqDataTCPAddr, err := net.ResolveTCPAddr("tcp", mc.ARQDataAddr)
		if err != nil {
			return fmt.Errorf("resolve ARQ data address: %w", err)
		}
		mc.ARQDataConn, err = net.DialTCP("tcp", nil, arqDataTCPAddr)
		if err != nil {
			return fmt.Errorf("connect to ARQ data port %s: %w", mc.ARQDataAddr, err)
		}
		logLines = append(logLines, fmt.Sprintf("Connected to ARQ Data: %s", mc.ARQDataAddr))
	} else {
		logLines = append(logLines, "ARQ Data port not specified or same as control, using control for data.")
		mc.ARQDataConn = mc.ARQControlConn
	}

	broadcastTCPAddr, err := net.ResolveTCPAddr("tcp", mc.BroadcastAddr)
	if err != nil {
		return fmt.Errorf("resolve Broadcast address: %w", err)
	}
	mc.BroadcastConn, err = net.DialTCP("tcp", nil, broadcastTCPAddr)
	if err != nil {
		return fmt.Errorf("connect to Broadcast port %s: %w", mc.BroadcastAddr, err)
	}
	logLines = append(logLines, fmt.Sprintf("Connected to Broadcast: %s", mc.BroadcastAddr))

	go mc.readARQControl()
	go mc.readARQData()
	go mc.readBroadcast()

	return nil
}

func (mc *ModemClient) SendCommand(cmd string) error {
	mc.mu.Lock()
	conn := mc.ARQControlConn
	quit := mc.quit
	mc.mu.Unlock()
	if conn == nil {
		return fmt.Errorf("not connected")
	}
	select {
	case mc.LogCh <- fmt.Sprintf("TX Command: %s", cmd):
	case <-quit:
		return fmt.Errorf("disconnected")
	}
	_, err := conn.Write([]byte(cmd + "\r"))
	return err
}

func (mc *ModemClient) ListenOn() error {
	return mc.SendCommand("LISTEN ON")
}

func (mc *ModemClient) ListenOff() error {
	return mc.SendCommand("LISTEN OFF")
}

// SendCQFrame queues a one-shot CQ frame advertising the source callsign and
// the given bandwidth (Hz).
func (mc *ModemClient) SendCQFrame(src string, bwHz int) error {
	return mc.SendCommand(fmt.Sprintf("CQFRAME %s %d", src, bwHz))
}

func (mc *ModemClient) ConnectARQ(src, dst string) error {
	mc.mu.Lock()
	if mc.ARQControlConn == nil {
		mc.mu.Unlock()
		return fmt.Errorf("not connected")
	}
	if mc.connectRespCh != nil {
		mc.mu.Unlock()
		return fmt.Errorf("ARQ connection already in progress")
	}

	mc.connectRespCh = make(chan string, 4)
	respCh := mc.connectRespCh
	quit := mc.quit
	mc.mu.Unlock()

	defer func() {
		mc.mu.Lock()
		mc.connectRespCh = nil
		mc.mu.Unlock()
	}()

	cmd := fmt.Sprintf("CONNECT %s %s\r", src, dst)
	mc.mu.Lock()
	_, err := mc.ARQControlConn.Write([]byte(cmd))
	mc.mu.Unlock()
	if err != nil {
		return fmt.Errorf("send CONNECT: %w", err)
	}

	timeout := time.After(120 * time.Second)
	for {
		select {
		case resp := <-respCh:
			if strings.HasPrefix(resp, "CONNECTED") {
				mc.mu.Lock()
				mc.arqConnected = true
				mc.mu.Unlock()
				mc.StatusCh <- "CONNECTED"
				return nil
			}
			if strings.HasPrefix(resp, "WRONG") {
				mc.StatusCh <- "WRONG"
				return fmt.Errorf("CONNECT rejected by server")
			}
			if strings.HasPrefix(resp, "BUSY") {
				mc.StatusCh <- "BUSY"
				return fmt.Errorf("channel busy")
			}
			if strings.HasPrefix(resp, "DISCONNECTED") {
				mc.mu.Lock()
				mc.arqConnected = false
				mc.mu.Unlock()
				mc.StatusCh <- "DISCONNECTED"
				return fmt.Errorf("disconnected before connection established")
			}
		case <-quit:
			return fmt.Errorf("disconnected")
		case <-timeout:
			return fmt.Errorf("connect timeout waiting for CONNECTED")
		}
	}
}

func (mc *ModemClient) DisconnectARQ() error {
	return mc.SendCommand("DISCONNECT")
}

func (mc *ModemClient) AbortARQ() error {
	return mc.SendCommand("ABORT")
}

func (mc *ModemClient) IsARQConnected() bool {
	mc.mu.Lock()
	defer mc.mu.Unlock()
	return mc.arqConnected
}

func (mc *ModemClient) SendARQData(data []byte) error {
	mc.mu.Lock()
	defer mc.mu.Unlock()
	if mc.ARQDataConn == nil || mc.ARQDataConn == mc.ARQControlConn {
		return fmt.Errorf("not connected to ARQ data port")
	}
	if !mc.arqConnected {
		return fmt.Errorf("ARQ session not established; wait for CONNECTED")
	}
	_, err := mc.ARQDataConn.Write(data)
	return err
}

func (mc *ModemClient) SendARQFile(filePath string) error {
	mc.mu.Lock()
	if mc.ARQDataConn == nil || mc.ARQDataConn == mc.ARQControlConn {
		mc.mu.Unlock()
		return fmt.Errorf("not connected to ARQ data port")
	}
	if !mc.arqConnected {
		mc.mu.Unlock()
		return fmt.Errorf("ARQ session not established; wait for CONNECTED")
	}
	mc.mu.Unlock()

	f, err := os.Open(filePath)
	if err != nil {
		return fmt.Errorf("open file: %w", err)
	}
	defer f.Close()

	buf := make([]byte, 1024)
	total := 0
	for {
		n, err := f.Read(buf)
		if n > 0 {
			mc.mu.Lock()
			if mc.ARQDataConn == nil {
				mc.mu.Unlock()
				return fmt.Errorf("data connection closed during transfer")
			}
			wn, werr := mc.ARQDataConn.Write(buf[:n])
			mc.mu.Unlock()
			if werr != nil {
				return fmt.Errorf("write data port: %w", werr)
			}
			if wn < n {
				return fmt.Errorf("short write: %d/%d bytes", wn, n)
			}
			total += wn
		}
		if err != nil {
			if err == io.EOF {
				break
			}
			return fmt.Errorf("read file: %w", err)
		}
	}
	mc.LogCh <- fmt.Sprintf("ARQ file sent: %s (%d bytes)", filePath, total)
	return nil
}

func (mc *ModemClient) SendBroadcast(data []byte) error {
	mc.mu.Lock()
	defer mc.mu.Unlock()

	if mc.BroadcastConn == nil {
		return fmt.Errorf("not connected to Broadcast port")
	}

	kissData := append([]byte{0x00}, data...)
	frame := KISSEncode(kissData)

	_, err := mc.BroadcastConn.Write(frame)
	if err != nil {
		return fmt.Errorf("send Broadcast: %w", err)
	}
	return nil
}

func (mc *ModemClient) IsConnected() bool {
	mc.mu.Lock()
	defer mc.mu.Unlock()
	return mc.ARQControlConn != nil && mc.BroadcastConn != nil
}

func (mc *ModemClient) Disconnect() {
	var disconnected []string

	mc.mu.Lock()
	control := mc.ARQControlConn
	if control != nil {
		control.Close()
		mc.ARQControlConn = nil
		disconnected = append(disconnected, "Disconnected from ARQ Control.")
	}
	if mc.ARQDataConn != nil && mc.ARQDataConn != control {
		mc.ARQDataConn.Close()
		mc.ARQDataConn = nil
		disconnected = append(disconnected, "Disconnected from ARQ Data.")
	}
	if mc.BroadcastConn != nil {
		mc.BroadcastConn.Close()
		mc.BroadcastConn = nil
		disconnected = append(disconnected, "Disconnected from Broadcast.")
	}

	mc.arqConnected = false

	close(mc.quit)
	mc.quit = make(chan struct{})
	mc.mu.Unlock()

	for _, msg := range disconnected {
		mc.LogCh <- msg
	}
}

func (mc *ModemClient) readARQControl() {
	mc.mu.Lock()
	conn := mc.ARQControlConn
	quit := mc.quit
	mc.mu.Unlock()
	if conn == nil {
		return
	}
	reader := bufio.NewReader(conn)
	for {
		select {
		case <-quit:
			return
		default:
			line, err := reader.ReadString('\r')
			if err != nil {
				if err != io.EOF {
					mc.LogCh <- fmt.Sprintf("ARQ Control read error: %v", err)
				}
				return
			}
			line = strings.TrimSpace(line)
			if line == "" {
				continue
			}

			mc.dispatchControlLine(line)
		}
	}
}

func (mc *ModemClient) dispatchControlLine(line string) {
	mc.IncomingARQCh <- line

	upper := strings.ToUpper(line)

	switch {
	case strings.HasPrefix(upper, "CONNECTED"):
		mc.mu.Lock()
		mc.arqConnected = true
		mc.mu.Unlock()
		mc.StatusCh <- "CONNECTED"

	case strings.HasPrefix(upper, "DISCONNECTED"):
		mc.mu.Lock()
		mc.arqConnected = false
		mc.mu.Unlock()
		mc.StatusCh <- "DISCONNECTED"

	case strings.HasPrefix(upper, "BUSY"):
		mc.StatusCh <- "BUSY"

	case strings.HasPrefix(upper, "PENDING"):
		mc.StatusCh <- "PENDING"

	case strings.HasPrefix(upper, "CANCELPENDING"):
		mc.StatusCh <- "CANCELPENDING"

	case strings.HasPrefix(upper, "CQFRAME"):
		mc.StatusCh <- "CQFRAME"
	}

	mc.mu.Lock()
	if mc.connectRespCh != nil {
		select {
		case mc.connectRespCh <- line:
		default:
		}
	}
	mc.mu.Unlock()
}

func (mc *ModemClient) readARQData() {
	mc.mu.Lock()
	conn := mc.ARQDataConn
	quit := mc.quit
	arqControlConn := mc.ARQControlConn
	mc.mu.Unlock()
	if conn == nil || conn == arqControlConn {
		return
	}

	buf := make([]byte, 4096)
	for {
		select {
		case <-quit:
			return
		default:
			n, err := conn.Read(buf)
			if err != nil {
				if err != io.EOF {
					mc.LogCh <- fmt.Sprintf("ARQ Data read error: %v", err)
				}
				return
			}
			if n > 0 {
				data := make([]byte, n)
				copy(data, buf[:n])
				mc.IncomingARQDataCh <- data
			}
		}
	}
}

func (mc *ModemClient) readBroadcast() {
	buffer := make([]byte, 4096)
	frameBuffer := &bytes.Buffer{}
	inFrame := false
	mc.mu.Lock()
	conn := mc.BroadcastConn
	quit := mc.quit
	mc.mu.Unlock()
	if conn == nil {
		return
	}

	for {
		select {
		case <-quit:
			return
		default:
			n, err := conn.Read(buffer)
			if err != nil {
				if err != io.EOF {
					mc.LogCh <- fmt.Sprintf("Broadcast read error: %v", err)
				}
				return
			}

			for i := 0; i < n; i++ {
				b := buffer[i]
				switch b {
				case FEND:
					if inFrame {
						decoded, err := KISSDecode(frameBuffer.Bytes())
						if err != nil {
							mc.LogCh <- fmt.Sprintf("KISS decode error: %v", err)
						} else {
							decoded = TrimKISSCommand(decoded)
							mc.IncomingBcastCh <- decoded
						}
						frameBuffer.Reset()
						inFrame = false
					} else {
						inFrame = true
						frameBuffer.Reset()
					}
				default:
					if inFrame {
						frameBuffer.WriteByte(b)
					}
				}
			}
		}
	}
}
