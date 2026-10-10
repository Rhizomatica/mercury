package client

import (
	"reflect"
	"testing"
)

// Issue #340: a peer ending lines with a bare CR (as many HF chat programs
// do) never reached the chat pane, since only "\n" ended a line.
func TestLineAssemblerEndings(t *testing.T) {
	cases := []struct {
		name   string
		chunks []string
		want   []string
		rest   string
	}{
		{"LF", []string{"hello\nworld\n"}, []string{"hello", "world"}, ""},
		{"bare CR", []string{"hello\rworld\r"}, []string{"hello", "world"}, ""},
		{"CRLF", []string{"hello\r\nworld\r\n"}, []string{"hello", "world"}, ""},
		{"CRLF split across pushes", []string{"hello\r", "\nworld\n"}, []string{"hello", "world"}, ""},
		{"line split across pushes", []string{"hel", "lo\r"}, []string{"hello"}, ""},
		{"blank lines dropped", []string{"\r\n\r\n  \nhi\n"}, []string{"hi"}, ""},
		{"no ending waits", []string{"no ending"}, nil, "no ending"},
	}
	for _, tc := range cases {
		var a lineAssembler
		var got []string
		for _, ch := range tc.chunks {
			got = append(got, a.push([]byte(ch))...)
		}
		if !reflect.DeepEqual(got, tc.want) {
			t.Errorf("%s: lines %q, want %q", tc.name, got, tc.want)
		}
		if a.pending() != (tc.rest != "") {
			t.Errorf("%s: pending %v, want %v", tc.name, a.pending(), tc.rest != "")
		}
		if f := a.flush(); f != tc.rest {
			t.Errorf("%s: flush %q, want %q", tc.name, f, tc.rest)
		}
		if a.pending() || a.flush() != "" {
			t.Errorf("%s: flush did not empty the assembler", tc.name)
		}
	}
}

// Bytes are never altered: every non-blank line comes out exactly as sent.
func TestLineAssemblerKeepsBytes(t *testing.T) {
	var a lineAssembler
	got := a.push([]byte("\x00\x01\xff ok\t\n"))
	if len(got) != 1 || got[0] != "\x00\x01\xff ok\t" {
		t.Fatalf("got %q", got)
	}
}
