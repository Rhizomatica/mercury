# Winlink gateways with Mercury

Mercury speaks the VARA TNC protocol (control on port 8300, data on 8301, see
[TNC.md](TNC.md)), so Winlink software that drives VARA HF can drive Mercury.
Several gateways run Mercury today. This page collects what their sysops and
users found out, in issues #527 and #554 (November 2026).

Winlink has no Mercury mode of its own yet. A Mercury gateway is listed as a
**VARA** channel, and the service code **`MERCURY`** is what tells users it
runs Mercury, not VARA.

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

RMS Trimode starts VARA from a fixed path and does not accept another program
in its place. The workaround that works:

1. Copy all of Mercury's files into the VARA folder.
2. Rename `VARA.exe` to `VARA_original.exe`.
3. Rename `mercury-ui.exe` to `VARA.exe`.

Trimode then starts Mercury, and its VARA line shows the gateway listening.

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

### Two Trimode instances (being tested)

To list Mercury under `MERCURY` and ARDOP under `PUBLIC`, a sysop is testing
two copies of Trimode on one radio: one for Mercury, one for ARDOP. Both
modems then listen at the same time, instead of taking turns.

Two programs cannot share one COM port. Mercury opens its PTT port when it
starts and keeps it open, with exclusive access on Windows, and ARDOP and
Trimode most likely do the same. So:

- turn off CAT control in both copies and set the radio to the channel by
  hand; and
- key each modem through a different port, or use the radio's DATA VOX so
  neither needs a port. Some radios give two virtual COM ports on one USB
  cable; the FTDX10, for example, gives "Enhanced" and "Standard".

## Known issues

- **Calls missed by a scanning gateway**: see above and #527.
- **`WRONG Last cmd=PUBLIC ON` in Trimode's log**: Mercury supports
  `PUBLIC ON`, so the `WRONG` is probably the answer to a different command.
  Mercury logs every command it rejects as
  `Command rejected (WRONG): <command>`; that line shows which one.

## Scanning BBS software (BPQ32, LinBPQ)

BPQ32 drives Mercury through its VARA driver too. The
[scanning hosts](TNC.md#scanning-hosts-bpq32) section of TNC.md describes how
Mercury signals an incoming call to a scanning host (`PENDING`,
`CANCELPENDING`) and how it handles `LISTEN OFF` and disconnects while the
host scans.
