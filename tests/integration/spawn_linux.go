//go:build linux

package integration

import (
	"os/exec"
	"runtime"
	"syscall"
)

// startChild starts a Mercury instance that cannot outlive this test binary.
//
// The Go test runner does not exit cleanly when a test exceeds -timeout: it
// dumps goroutines and calls os.Exit, so t.Cleanup, defers and context
// cancellation never run and every child Mercury is orphaned.  Those orphans
// keep their audio threads spinning, and once a few have accumulated they
// starve the harness's real-time pacing until runs stop completing at all —
// which looks exactly like a Mercury connect bug (CALL sent once, never
// retried, peer stuck in ACCEPTING) and was chased as one more than once.
//
// PR_SET_PDEATHSIG makes the kernel signal the child when its parent dies,
// whatever the parent's exit path.  The signal is delivered when the creating
// THREAD exits, not the process, so the goroutine is pinned to its OS thread
// across the fork: without that the Go runtime may retire the thread and kill
// a perfectly healthy child.
func startChild(cmd *exec.Cmd) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	if cmd.SysProcAttr == nil {
		cmd.SysProcAttr = &syscall.SysProcAttr{}
	}
	cmd.SysProcAttr.Pdeathsig = syscall.SIGKILL
	return cmd.Start()
}
