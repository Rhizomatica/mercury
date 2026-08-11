//go:build !windows

package main

import (
	"os/exec"
	"syscall"
)

// detach runs the launched Mercury Client in its own session, so it is not tied
// to this process's terminal and keeps its own window and lifecycle.
func detach(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
}
