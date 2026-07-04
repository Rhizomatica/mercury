package main

import (
	"encoding/binary"
	"math"
	"testing"
)

func TestParseStatusMessage(t *testing.T) {
	payload := []byte(`{"type":"status","bitrate":1200,"snr":6.5,"sync":true,"direction":"tx","client_tcp_connected":true,"bytes_transmitted":34,"bytes_received":900,"tx_gain_db":3.5,"tx_peak_dbfs":-2.1,"waterfall":true}`)

	status, err := parseStatusMessage(payload)
	if err != nil {
		t.Fatalf("parseStatusMessage returned error: %v", err)
	}
	if status.Bitrate != 1200 {
		t.Fatalf("expected bitrate 1200, got %d", status.Bitrate)
	}
	if math.Abs(status.SNR-6.5) > 0.001 {
		t.Fatalf("expected snr 6.5, got %v", status.SNR)
	}
	if !status.Sync {
		t.Fatal("expected sync to be true")
	}
	if status.Direction != "tx" {
		t.Fatalf("expected direction tx, got %q", status.Direction)
	}
	if status.Waterfall != true {
		t.Fatal("expected waterfall to be true")
	}
}

func TestBuildWebSocketURLUsesWebsocketPath(t *testing.T) {
	got := buildWebSocketURL("ws", "127.0.0.1", "10000")
	want := "ws://127.0.0.1:10000/websocket"
	if got != want {
		t.Fatalf("expected %q, got %q", want, got)
	}
}

func TestParseSpectrumFrame(t *testing.T) {
	frame := make([]byte, 8+4*4)
	frame[0] = 0x59
	frame[1] = 0x52
	frame[2] = 0x43
	frame[3] = 0x4D
	binary.LittleEndian.PutUint16(frame[4:], 4)
	binary.LittleEndian.PutUint16(frame[6:], 8000)
	binary.LittleEndian.PutUint32(frame[8:], 0x3F800000)
	binary.LittleEndian.PutUint32(frame[12:], 0x40000000)
	binary.LittleEndian.PutUint32(frame[16:], 0x40400000)
	binary.LittleEndian.PutUint32(frame[20:], 0x40800000)

	spectrum, sampleRate, err := parseSpectrumFrame(frame)
	if err != nil {
		t.Fatalf("parseSpectrumFrame returned error: %v", err)
	}
	if sampleRate != 8000 {
		t.Fatalf("expected sample rate 8000, got %d", sampleRate)
	}
	if len(spectrum) != 4 {
		t.Fatalf("expected 4 spectrum bins, got %d", len(spectrum))
	}
	if math.Abs(float64(spectrum[0])-1.0) > 0.001 {
		t.Fatalf("expected first bin 1.0, got %v", spectrum[0])
	}
}
