#!/usr/bin/env python3
"""onair_logs.py <sender journal> <receiver journal>: one on-air carousel run, per round.

The two files are the Mercury journals of the two stations for the length of
one transfer, e.g.  journalctl -u modem --since @<start> > gw.log  on each, with
Mercury running verbose (debug lines).  The station clocks should agree to a few
tenths of a second (NTP).  Prints:

  - how the sender's rounds were answered: by a pattern or by a poll, and the
    median turnaround after each (sender unkey -> next keydown);
  - which rungs carried the data, and how often the rung changed;
  - keydowns of the two stations that overlapped on the air, with times --
    each one is two transmitters keyed at once on a half-duplex channel;
  - with --trace, the receiver's carousel decision trace (debug log component
    "carousel"), repeats folded.
"""
import re
import statistics
import sys

STAMP = re.compile(r'(\d\d):(\d\d):(\d\d\.\d+) \[\+[0-9.]+s\] \[(\w+)\] \[([\w-]+)\] (.*)')


def lines(path):
    for raw in open(path, errors='replace'):
        m = STAMP.search(raw)
        if m:
            t = int(m[1]) * 3600 + int(m[2]) * 60 + float(m[3])
            yield t, f"{m[1]}:{m[2]}:{m[3][:6]}", m[5], m[6]


def keydowns(path):
    out, on = [], None
    for t, hms, comp, msg in lines(path):
        if comp == 'radio' and msg.startswith('TX enabled'):
            on = (t, hms)
        elif comp == 'radio' and msg.startswith('TX disabled') and on:
            out.append((on[0], t, on[1]))
            on = None
    return out


def rounds(sender):
    """Sender unkey -> next keydown, split by what answered the round."""
    gaps = {'pattern': [], 'poll': []}
    off = kind = on = None
    for t, _, comp, msg in lines(sender):
        if comp == 'radio' and msg.startswith('TX enabled'):
            if off is not None and kind and t - off < 40:
                gaps[kind].append(t - off)
            on, kind = t, None
        elif comp == 'radio' and msg.startswith('TX disabled') and on is not None:
            off = t if t - on > 6 else None        # a data round, not a control frame
            on = None
        elif 'Pattern ACK detected' in msg and off is not None:
            kind = 'pattern'
        elif 'Decoded frame mode=23' in msg and off is not None and kind is None:
            kind = 'poll'
    return gaps


def rungs(receiver):
    modes = [re.search(r'\((\w+)\)', msg)[1] for _, _, comp, msg in lines(receiver)
             if comp == 'modem-rx' and 'Decoded frame' in msg and 'DATAC16' not in msg]
    runs, prev = [], None
    for m in modes:
        if m != prev:
            runs.append([m, 0])
            prev = m
        runs[-1][1] += 1
    return runs


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 2:
        sys.exit(__doc__)
    sender, receiver = args
    gaps = rounds(sender)
    med = lambda v: f"{statistics.median(v):.1f} s" if v else "-"
    n_pat, n_poll = len(gaps['pattern']), len(gaps['poll'])
    share = f" ({100 * n_pat // (n_pat + n_poll)} %)" if n_pat + n_poll else ""
    print(f"rounds answered by a pattern: {n_pat}{share}, turnaround {med(gaps['pattern'])}; "
          f"by a poll: {n_poll}, turnaround {med(gaps['poll'])}")
    r = rungs(receiver)
    print("rungs: " + " | ".join(f"{m} x{k}" for m, k in r) + f"   ({max(len(r) - 1, 0)} changes)")
    a, b = keydowns(sender), keydowns(receiver)
    ov = [(max(s0, r0), min(s1, r1), sh, rh, s1 - s0, r1 - r0)
          for s0, s1, sh in a for r0, r1, rh in b if min(s1, r1) > max(s0, r0)]
    print(f"keydowns: sender {len(a)} ({sum(y - x for x, y, _ in a):.0f} s), "
          f"receiver {len(b)} ({sum(y - x for x, y, _ in b):.0f} s); "
          f"overlapping: {len(ov)} ({sum(y - x for x, y, *_ in ov):.1f} s)")
    for x, y, sh, rh, sl, rl in ov:
        print(f"   sender {sh} for {sl:.1f} s  x  receiver {rh} for {rl:.1f} s  ({y - x:.1f} s on top)")
    if '--trace' in sys.argv:
        prev, count, first = None, 0, None
        fold = lambda s: re.sub(r'snr=[-0-9.]+|gp\[.*', '', s) if s else None
        for _, hms, comp, msg in lines(receiver):
            if comp != 'carousel':
                continue
            if fold(msg) == fold(prev):
                count += 1
                prev = msg
                continue
            if prev is not None:
                print(f"{first}  {prev}" + (f"   (x{count})" if count > 1 else ""))
            prev, count, first = msg, 1, hms
        if prev is not None:
            print(f"{first}  {prev}" + (f"   (x{count})" if count > 1 else ""))


if __name__ == '__main__':
    main()
