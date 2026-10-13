# JNOS 2.0 over Mercury

[JNOS 2.0](https://github.com/Rhizomatica/jnos2) (Maiko Langelaar, VE4KLM) is a
packet-radio network operating system: BBS, SMTP/POP3, NNTP, store-and-forward
mail between BBSes and with Winlink, AX.25, NET/ROM and its own TCP/IP stack.
It drives Mercury through its existing **VARA driver**, on Mercury's
VARA-compatible TNC port (8300 control, 8301 data). No Mercury-specific code
is involved.

This page records how JNOS was set up on the bench pair and what was tested on
2026-10-12:

- **BBS to BBS over the air**: works;
- **telnet from one station to the other station's Linux**, carried as a byte
  stream in a Mercury session, **with no IP on the air**: works, in both
  directions;
- **IP over the air (PPP over VARA)**: tried, not pursued -- not needed for the
  above.

Maiko's own write-up of his JNOS and Mercury setup, which he and Gary (K7EK)
have used to pass mail since September 2026, is at
`https://www.langelaar.net/sec/cb378e4bb0947000/radio/jnos2/mercury/`.

## How the pieces connect

```
 gateway (PU2UIT, Pi 4, sBitx)                     estacao2 (PU2UIT-2, Pi 5, sBitx)

 Linux telnet ─┐                                                ┌─ Linux telnetd
               │ jnostun (192.168.44.1 ↔ .2)   (192.168.45.1 ↔ .2) jnostun │
            JNOS ── TCP 8300/8301 ── Mercury ~~ HF ~~ Mercury ── TCP 8300/8301 ── JNOS
            (own IP stack)                                          (own IP stack)
```

- **JNOS has its own TCP/IP stack.** `127.0.0.1` in a JNOS config is *JNOS's*
  loopback, not the host's. JNOS reaches Mercury, and the station's Linux
  reaches JNOS, through a tun interface: `jnostun`. Nothing on `jnostun` goes
  on the air. It is a point-to-point link between JNOS and its own host.
- **On the air there is only a Mercury session** carrying a byte stream: BBS
  text, or the telnet bytes. JNOS opens the session with the TNC `CONNECT`
  command, and the far JNOS decides what to hand it to by the callsign that
  was called.

## Callsign plan

JNOS gives Mercury all its callsigns in one `MYCALL`, and acts on the one that
was called:

| Station | Interface (`ax25 mycall`) | BBS (`ax25 bbscall`) | Telnet to this Linux (`ax25 tncall`) | JNOS on `jnostun` |
|---|---|---|---|---|
| gateway | PU2UIT-10 | **PU2UIT** | PU2UIT-13 | 192.168.44.2 |
| estacao2 | PU2UIT-11 | **PU2UIT-2** | PU2UIT-12 | 192.168.45.2 |

- **The BBS call must differ from the interface call.** A call to the
  interface call is a plain link: the remote BBS waits for the caller to speak
  first. JNOS's gateway (`C vara ...` from a BBS) waits for the remote to speak
  first. Both wait forever. A call to the `bbscall` "jumpstarts" the BBS, which
  sends its banner at once. Maiko's own config does the same (`mycall ve4klm-1`,
  `bbscall ve4klm`).
- `tncall` links a call straight to a TCP service, here the station's Linux
  telnetd. It must be configured **before** `attach vara`. JNOS builds its list
  of own callsigns once, at the attach. A `tncall` added later is registered
  with Mercury if you resend `MYCALL`, but JNOS itself then reads an incoming
  call to it as an *outgoing* call and aborts it.

## Setup

### Mercury

Both stations ran trunk `6d6998b` (protocol version 2) as their modem
service. `uucpd` (with `uucp.socket`) holds Mercury's TNC ports on a HERMES
station, so it was stopped for the test, and so was the gateway's
`caller.service`.

### Build JNOS

From our fork, natively on each Pi (gcc, make and `libncurses-dev` are
enough):

```
git -C jnos2 archive --prefix=jnos-src/ -o jnos-src.tar HEAD:2.0q   # on a workstation
tar xf jnos-src.tar && cd jnos-src
make clean            # the repo tracks 241 prebuilt .o files: do not link them
./configure && make -j4
```

`TNCALL` (`ax25 tncall`) is already enabled in the shipped `config.h`.
`make clean` also removes `configure.okay`, so run `./configure` again after
it; otherwise `make` stops at "you have to run './configure' first".

Our fork's `main` (from `4850ff3`) adds two fixes found on this bench:
- **`vara bw <interface> [500|2300|2750]`.** The bandwidth JNOS asks of the
  modem was fixed at BW500, and the default still is.
- **The VARA transmit process survives a data-channel error.** Before, one
  failed write ended it for good.

`jnos-2.0q-vara-bw-and-tx.patch`, the same change relative to the 2.0q
source, is being offered to Maiko upstream.

### Runtime tree

Start from Maiko's test environment, `jnos2/mercury/mercnos.tar.gz`, with his
callsign replaced:

```
mkdir -p /home/pi/jnos/rte && cd /home/pi/jnos/rte
tar xzf mercnos.tar.gz
find . -type f -exec sed -i 's/ve4klm/pu2uit/g; s/VE4KLM/PU2UIT/g' {} \;
cp /path/to/jnos-src/jnos .
```

**Replace its `ftpusers`.** It carries real accounts with passwords: Maiko's,
and K7EK's. Keep one sysop account for your own call with a password of your
own:

```
echo "pu2uit <password> /home/pi/jnos/rte/public 0x0405f" > ftpusers
chmod 600 ftpusers
```

**A login name must be a bare callsign.** JNOS checks it as a callsign
(`MBX_CALLCHECK`) and rejects an SSID (`pu2uit-2`: "Login incorrect", logged as
"bad login"). On the bench the gateway's account is `pu2uit` and estacao2's is
`cr7byq`, an operator's own call. Each has its own password, in that station's
`ftpusers`.

### `jnostun`

A persistent tun owned by the user who runs JNOS, so JNOS needs no root:

```
sudo ip tuntap add dev jnostun mode tun user pi
sudo ip addr add 192.168.44.1 peer 192.168.44.2 dev jnostun   # 192.168.45.x on estacao2
sudo ip link set jnostun up
```

Give each station its own subnet: the two hosts then stay distinct if their
networks are ever routed together. The stations' own `tun0`/`tun1` belong to
other things (the VPN), so do not reuse them.

### `autoexec.nos` (gateway; estacao2 swaps the calls and uses 192.168.45.x)

```
log on
ip address 10.44.0.1
hostname pu2uit
# the interface call; the BBS answers on the station call (bbscall), so a call to it starts the BBS
ax25 mycall pu2uit-10
ax25 bbscall pu2uit
# calls to PU2UIT-13 are linked to this station's Linux telnetd (inetd, bound to 192.168.44.1)
ax25 tncall pu2uit-13 192.168.44.1 23
attach tun jtun 1500 0 jnostun
ifconfig jtun ipaddress 192.168.44.2
ifconfig jtun netmask 255.255.255.255
route add 192.168.44.1 jtun
attach vara vara 256 192.168.44.1 8300
# ask the modem for 2300 Hz: BW500 keeps a Mercury session on its narrow modes (needs jnos2 4850ff3)
vara bw vara 2300
ifconfig vara ipaddress 10.44.0.1
vara cmddebug on
vara debug on
start finger
start telnet
start vara
```

On attach, JNOS sends Mercury `VERSION`, `MYCALL PU2UIT-10 PU2UIT PU2UIT-13`,
the bandwidth (`BW2300` with `vara bw vara 2300`, `BW500` without it),
`COMPRESSION ON` and `LISTEN ON`. Mercury answers with a `REGISTERED` for each
callsign.

### Linux telnetd for `tncall`

```
sudo apt-get install telnet inetutils-telnetd
```

In `/etc/inetd.conf`, enable the telnet line **bound to the `jnostun` address
only**:

```
192.168.44.1:telnet	stream	tcp	nowait	root	/usr/sbin/tcpd	/usr/sbin/telnetd
```

Then `sudo systemctl restart inetutils-inetd`. `ss -tlnp | grep :23` must show
`192.168.44.1:23` and nothing else. The server cannot be reached from the LAN
or the VPN, and if `jnostun` is absent it simply does not listen.

### Running JNOS without a terminal

JNOS's console needs a terminal. On the bench it runs under `script`, with its
input appended to a file:

```
cd /home/pi/jnos/rte && : > cmd.in
TERM=vt100 setsid -f bash -c 'tail -f cmd.in | script -qfec "stty rows 50 cols 140; ./jnos -d /home/pi/jnos/rte" console.log'
printf 'tcp status\r' >> cmd.in     # a console command; output in console.log
```

**Stop JNOS with `exit` on its console** (`printf 'exit\ry' >> cmd.in`), never
with a signal. A killed JNOS cannot close its TCP connections to Mercury,
because they live in JNOS's own IP stack. Mercury then keeps the dead client
and refuses the next one until it is restarted. The kernel may also still hold
the closing state for the same address pair. A new JNOS reuses local ports
1024/1025, so it answers that state with an RST and waits a retransmission
timeout before trying again.

## Tests

All on 7.050 MHz USB into dummy loads, gateway at 20 % power, 2026-10-12.
"BBS" means JNOS's mailbox. Every test was driven with Debian's stock `telnet`
client, its input piped in on a timer.

### 1. BBS to BBS — works

On the gateway's Linux, `telnet 192.168.44.2`, log in to the gateway's BBS,
then `C vara pu2uit-2`:

```
Area: pu2uit (#0) >
Trying...  The escape character is: CTRL-T
*** connected to PU2UIT-2
[JNOS-2.0q.1-B1FHIM$]
You have 0 messages.
Area: pu2uit Current msg# 0.
?,A,B,C,CONV,D,E,F,H,I,IH,IP,J,K,L,M,N,NR,O,P,PI,R,S,T,U,V,W,X,Z >
No messages
...
Thank you pu2uit, for calling pu2uit-2 JNOS.
*** reconnected to PU2UIT-10
Area: pu2uit (#0) >
```

The `B` (bye) at estacao2's BBS returns the user to the gateway's own BBS.

### 2. Telnet to the other station's Linux — works, no IP on the air

On the gateway's Linux, `telnet 192.168.44.2`, log in to the gateway's BBS,
then `C vara pu2uit-12`. estacao2's JNOS links the call to its own Linux
telnetd:

```
Area: pu2uit (#0) >
Trying...  The escape character is: CTRL-T
*** connected to PU2UIT-12

Linux 6.18.50+rpt-rpi-v8 (estacao2.hermes.radio) (pts/1)

estacao2 login:
```

The test stopped at the login prompt on purpose. Telnet is clear text, and so
is everything on the air, so log in to a station's Linux this way only with an
account whose password you do not mind being heard.

The reverse direction works the same way. On estacao2's Linux,
`telnet 192.168.45.2`, log in to estacao2's BBS, then `C vara pu2uit-13`:

```
Trying...  The escape character is: CTRL-T
*** connected to PU2UIT-13

Linux 6.18.39+rpt-rpi-v8 (estacao.hermes.radio) (pts/1)

estacao login:
```

### 3. BBS at 2300 Hz — works

The same BBS session as test 1, with `vara bw vara 2300` on both stations (the
patched JNOS). Mercury received `BW2300`, and the session came up as
`CONNECTED PU2UIT-15 PU2UIT-2 2300`. The remote BBS's help text (`H`) came back
on QAM16C2, and the gateway sent on DATAC1. Neither mode is allowed in a BW500
session, which stops at DATAC3.

### 4. IP over the air (PPP over VARA) — not pursued

`vara pppmode vara on` turns the VARA interface into a PPP link. JNOS sources
`ppp.nos` when Mercury reports `CONNECTED`, to open LCP and IPCP. The radio
connect worked, and the called side brought PPP up. Two things stood in the
way, and the tests above do not need IP:

- In PPP mode `c vara <call>` is refused ("not an AX25 or VARA type
  interface").
- A raw `vara command vara "CONNECT A B"` connects, but no JNOS socket is
  waiting for it, so JNOS aborts the session at once ("connection succeeded for
  unknown socket").

Maiko's notes call PPP over VARA a working but high-latency IP bridge. His
demonstration pages are no longer online.

## Trying it

With JNOS running on both stations (`/home/pi/jnos/jnos-restart.sh` starts or
restarts it):

| From | Do | Reach |
|---|---|---|
| the gateway's Linux | `telnet 192.168.44.2`, log in as `pu2uit`, then `C vara pu2uit-2` | estacao2's BBS |
| the gateway's Linux | the same, then `C vara pu2uit-12` | estacao2's Linux login |
| estacao2's Linux | `telnet 192.168.45.2`, log in as `cr7byq`, then `C vara pu2uit` | the gateway's BBS |
| estacao2's Linux | the same, then `C vara pu2uit-13` | the gateway's Linux login |

- `B` leaves a remote BBS and returns you to your own. Ctrl-T is the BBS
  gateway's escape character.
- Expect tens of seconds per turn: every line is a Mercury keydown. Without
  `vara bw vara 2300`, JNOS asks for BW500 and the session stays at DATAC3
  or below.
- The BBS passwords are in each station's `/home/pi/jnos/rte/ftpusers`.

**Giving the stations back to HERMES.** While JNOS holds Mercury's TNC ports,
`uucpd` cannot, so the station carries no HERMES mail, and the gateway's
`caller` is stopped. `/home/pi/jnos/hermes-restore.sh` on each station stops
JNOS cleanly, returns the modem service to the station's normal Mercury, and
restarts `uucpd` (and `caller` on the gateway). It leaves `jnostun` and the
`jnostun`-only telnetd in place for the next test.

## Problems found

### In Mercury

- **Stock JNOS asks for BW500 and cannot be told otherwise.**
  `vara_initinterface()` hard-codes it. Mercury sets a session's bandwidth to
  the narrower of the two ends, and since 1.9.18 BW500 also caps the rungs at
  DATAC3. So **every session with a stock JNOS end stops at DATAC3**, even on a
  link that carries QAM16C2. Through 1.9.17 BW500 was not enforced. **Fixed in
  our fork** with `vara bw` (above). Both ends need it: one BW500 end narrows
  the whole session.
- **The CALL carries the primary `MYCALL`, not `CONNECT`'s source.**
  `CONNECT PU2UIT-15 PU2UIT-2` went out as PU2UIT-10, and estacao2 reported
  `CONNECTED PU2UIT-10 PU2UIT-2`. The calling side still echoes
  `CONNECTED PU2UIT-15 ...`, so the two ends disagree on the caller. VARA sends
  the source given, when it is registered. Nothing broke here, but a peer that
  checks the caller sees a different call than the one the caller logs.

### In JNOS (2.0q.1)

- **A failed write to the data port ends VARA transmit for good.** On
  `vara_sendstream()` returning -1, `vara_tx()` returns and is never restarted:
  the interface receives but never sends again until JNOS restarts. It was hit
  here after a killed JNOS left stale TCP state behind. **Fixed in our fork:**
  the buffer is dropped and logged, and transmission carries on. The station
  kernel cannot kill a socket on demand (`ss -K`), so this was not reproduced
  deliberately.
- **`127.0.0.1` in `attach vara` is JNOS's own loopback.** Mercury on the same
  host must be reached through a tun (above).
- **The attach is tried only briefly.** If Mercury is not listening yet, JNOS
  gives up. Start JNOS after Mercury's port 8300 is open.

### In the test setup

- **`pkill -f <pattern>` run over `ssh` matches its own command line** and
  kills the shell running it. Anchor the patterns: `pgrep -f '^\./jnos -d'`.
- **The stock telnet client turns `\n` into CR LF.** Piped input must end its
  lines with `\n` alone. `\r\n` sends an empty extra line, which the login takes
  as the password; the real password then becomes the next login name and lands
  in JNOS's log. It happened once here: that test password was replaced, and the
  log line redacted.
