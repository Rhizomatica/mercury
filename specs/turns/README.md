# specs/turns

The turn rules of `docs/CAROUSEL-TURNS.md`, timed: who may key and when, so
that the two stations never transmit together, under any loss.

    java -cp /opt/TLA+Toolbox/tla2tools.jar tlc2.TLC -workers 6 \
        -metadir /tmp/tlc-turns -config CarouselTurns.cfg CarouselTurns.tla

- `CarouselTurns.cfg`: OFDM-like timing.  No error, 18.5 M distinct states.
- `CarouselTurns_mfsk.cfg`: a slow decode and long answers.  No error,
  27.1 M distinct states.
- `Mut_*.cfg`: one rule broken each (`nogrid`, `late`, `long`, `blind`,
  `reuse`).  Each must report `Invariant NoOverlap is violated`.

Give each TLC run its own `-metadir`: runs sharing a `states/` directory
delete each other's state.
