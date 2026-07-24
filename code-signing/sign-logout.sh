#!/bin/bash
# sign-logout.sh — tear down the persistent SimplySign session (SimplySign
# Desktop + Xvfb + fluxbox) started by sign.sh.  Safe if nothing is running.
set -uo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=sign-lib.sh
source "$DIR/sign-lib.sh"
ss_logout
