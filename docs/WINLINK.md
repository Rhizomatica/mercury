# Winlink gateways with Mercury

Mercury speaks the VARA TNC protocol (control on port 8300, data on 8301, see
[TNC.md](TNC.md)), so Winlink software that drives VARA HF can drive Mercury.
Several gateways run Mercury today. This page collects what their sysops and
users found out, in issues #527 and #554 (November 2026).

Winlink has no Mercury mode of its own yet. A Mercury gateway is listed as a
**VARA** channel, and the service code **`MERCURY`** is what tells users it
runs Mercury, not VARA.

Over the air, a Mercury gateway works like a VARA HF one: the client calls the
gateway, Mercury opens an ARQ connection, and the Winlink software exchanges
messages through Mercury's data port, as it would through VARA's. Both ends must
run Mercury: a Mercury gateway cannot connect to a client running VARA, nor the
reverse. On the client side, use the VARA HF session type (Winlink Express) or
transport (Pat), with Mercury running in VARA's place.

## Finding Mercury gateways

- **Winlink map** (https://winlink.org/RMSChannels): select **VARA** and enter
  service code **`MERCURY`**.
- **Pat**: `pat rmslist` only lists `PUBLIC` gateways by default. Add
  `MERCURY` to `service_codes` in Pat's `config.json`:

  ```json
  "service_codes": ["PUBLIC", "MERCURY"]
  ```

  Then connect with Pat's VARA HF transport, pointed at Mercury's TNC ports.
- **DigiPi** searches `PUBLIC` and `MERCURY`, so Mercury gateways appear when
  VARA HF is selected, with no change needed.

## Running a gateway with RMS Trimode (Windows)

RMS Trimode starts the VARA program it was set up with. When it cannot find
that program, it asks for it, and it accepts Mercury instead. This way Mercury
stays where its installer put it (found by YO4CIL, in #563):

1. In Trimode, set up VARA as usual (for example `C:\VARA\VARA.exe`), then
   close Trimode.
2. Rename `C:\VARA\VARA.exe` to `VARA_original.exe`.
3. Open Trimode again. It does not find `VARA.exe` and asks where the VARA
   program is. Select `C:\Program Files\MercuryHF\mercury-ui.exe` and press
   **Open**.

Trimode then starts Mercury, and its VARA line shows the gateway listening. To
go back to VARA, rename `VARA_original.exe` to `VARA.exe` and point Trimode at
it again.

An earlier workaround also works, but mixes Mercury's files with VARA's: copy
all of Mercury's files into the VARA folder, rename `VARA.exe` to
`VARA_original.exe`, and rename `mercury-ui.exe` to `VARA.exe`.

### Channel settings

- **Use one channel entry, with VARA and ARDOP both enabled on it.** Trimode
  stays on each channel entry for a few seconds (4 s in the setup we saw) and
  is deaf for 500 ms while switching. A Mercury call frame is 3.74 s long, and
  later calls are longer, so with more than one entry most calls are missed.
  This is being worked on in #527.
- **Service code `MERCURY`.** Trimode takes one service code per channel:
  `PUBLIC MERCURY` does not work. The code applies to everything on that
  channel, so ARDOP on the same entry is listed under `MERCURY` too, and
  clients that search only `PUBLIC` do not see it.

### Two Trimode instances

To list Mercury under `MERCURY` and ARDOP under `PUBLIC`, run two copies of
Trimode on one radio: one for Mercury, one for ARDOP. Both modems then listen
at the same time, instead of taking turns. YO4CIL runs it this way (#554).

Mercury opens its PTT serial port when it starts and keeps it open, with
exclusive access on Windows (`radio_io/serial_ptt.c`), so no other program can
open that port while Mercury runs. Keying both copies with RTS on the same COM
port did not work in that test. So:

- turn off CAT control in both copies and set the radio to the channel by
  hand; and
- use the radio's DATA VOX, so neither modem needs a port. This is what
  works: on YO4CIL's radio, with the VOX delay at its minimum, 30 ms.

Keying each modem through its own port should also work, but has not been
tried. Some radios give two virtual COM ports on one USB cable; the FTDX10,
for example, gives "Enhanced" and "Standard".

## Known issues

- **Calls missed by a scanning gateway**: see above and #527.
- **`WRONG Last cmd=PUBLIC ON` in Trimode's log**: Mercury supports
  `PUBLIC ON`, so the `WRONG` is probably the answer to a different command.
  Mercury logs every command it rejects as
  `Command rejected (WRONG): <command>`; that line shows which one. The
  Mercury window writes it to `mercury_engine.log`, in the `logs` folder next
  to `mercury-ui.exe` on Windows (see also [Logging](ARQ.md#logging)).

## Scanning BBS software (BPQ32, LinBPQ)

BPQ32 drives Mercury through its VARA driver too. The
[scanning hosts](TNC.md#scanning-hosts-bpq32) section of TNC.md describes how
Mercury signals an incoming call to a scanning host (`PENDING`,
`CANCELPENDING`) and how it handles `LISTEN OFF` and disconnects while the
host scans.
