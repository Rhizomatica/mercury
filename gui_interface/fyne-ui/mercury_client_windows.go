//go:build windows

package main

import (
	"os/exec"
	"syscall"
)

// DETACHED_PROCESS (0x00000008) is not exposed by Go's syscall package for
// Windows; CREATE_NEW_PROCESS_GROUP is. Define the missing flag here so the
// launched Client gets no console and survives this process exiting.
const detachedProcess = 0x00000008

// detach runs the launched Mercury Client as a detached process group, so it
// gets its own window and keeps running if this process exits.
func detach(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{
		CreationFlags: syscall.CREATE_NEW_PROCESS_GROUP | detachedProcess,
	}
}
