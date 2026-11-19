# Security policy

Mercury keys radio transmitters and often runs on unattended stations
(Raspberry Pi, sBitx, Winlink gateways), sometimes as root.  Please report
security problems privately, not in a public issue, pull request or the
mailing list.

## Reporting a vulnerability

Email the maintainers with `[SECURITY] Mercury` in the subject:

- Rafael Diniz <rafael@rhizomatica.org>
- Pedro Messetti <pedromessetti@rhizomatica.org>

If GitHub's
[private vulnerability reporting](https://github.com/Rhizomatica/mercury/security/advisories/new)
is enabled on this repository, you can use that instead.

Please include:

- the affected version or commit, and the platform (Linux/ALSA, Windows,
  macOS, Android, iOS, Sailfish OS, ...);
- which interface is involved (TCP TNC ports, the UI WebSocket, over the
  air, a config file, the installer, CI);
- steps or a proof of concept that reproduces it, and the impact you see;
- Mercury logs from the session, if you have them (`mercury -v -L
  <path>` writes a full log to a file).  Remove callsigns, addresses or
  anything else you don't want to share.

The TCP TNC ports and the UI WebSocket have no authentication by design:
they are meant for this machine or a trusted network.  The UI WebSocket
also accepts connections from any web origin, because the web app connects
cross-origin.  If a station's ports are reachable from untrusted networks,
restrict them with a host firewall (for example nftables).  That a client
can reach them and send commands without credentials is expected, not a
vulnerability on its own.  Memory-safety bugs in how they parse input, or
a way around a restriction Mercury does enforce, are worth reporting.

This is a volunteer-run project: reports are handled on a best-effort
basis, and no response time or fix timeline is promised.  Please report
privately and give us reasonable time before publishing details.

Anyone with specific security requirements (guaranteed response times,
coordinated disclosure, audits) can contact Rhizomatica about a support
contract.

## Supported versions

Security fixes go into the `mercuryv2` branch and the next release
(currently the `v1.9.x` series).  Only the latest release and `mercuryv2`
are supported; if you run an older release, upgrade to the latest one.
The `mercuryv1` branch is not supported.
