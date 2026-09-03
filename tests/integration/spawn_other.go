//go:build !linux

package integration

import "os/exec"

// startChild starts a Mercury instance.
//
// PR_SET_PDEATHSIG is Linux-only, so elsewhere this is a plain Start and an
// orphan can outlive a timed-out test run.  See the linux build of this file
// for what that costs and why it matters.
func startChild(cmd *exec.Cmd) error {
	return cmd.Start()
}
