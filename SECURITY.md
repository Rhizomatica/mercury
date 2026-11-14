# Security policy

Mercury keys radio transmitters and often runs on unattended stations
(Raspberry Pi, sBitx, Winlink gateways), sometimes as root.  Please report
security problems privately, not in a public issue, pull request or the
mailing list.

## Reporting a vulnerability

Email the maintainer, Rafael Diniz, at <rafael@riseup.net> with
`[SECURITY] Mercury` in the subject.  If GitHub's
[private vulnerability reporting](https://github.com/Rhizomatica/mercury/security/advisories/new)
is enabled on this repository, you can use that instead.

Please include:

- the affected version or commit, and the platform (Linux/ALSA, Windows,
  macOS, Android, ...);
- which interface is involved (TCP TNC ports, the UI WebSocket, over the
  air, a config file, the installer, CI);
- steps or a proof of concept that reproduces it, and the impact you see.

Please allow reasonable time for a fix to be released before you publish
details.

## Supported versions

Fixes go into the `mercuryv2` branch and the next release.
