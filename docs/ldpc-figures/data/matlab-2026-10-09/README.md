# Dated record: the MATLAB-run sweep of 2026-10-09

These files were written by a MATLAB script (R2026a, Communications
Toolbox) that is not part of the repository. They are kept as data, not as
a test, because Mercury's tests are the C unit tests and the Go bench, and
its evidence must be reproducible with free software. The reproducible
versions of the same measurements are the files one level up, written by
`utils/golden/ldpc_sweep` (see `docs/LDPC-DECODER.md`).

- `ldpc_fer_<code>.csv`: frame error rate against Es/N0 (dB), BPSK over
  AWGN, 600 frames per point or 30 errors per variant, Eb/N0 0 to 4 dB in
  0.5 dB steps. The eleven `C_*` columns are the C decoder driven by MATLAB
  on MATLAB-generated frames (legacy loop at caps 10, 50, 100; context
  decoders `sp`, `spt`, `nms`, `nms16` at caps 50 and 100; `_fixedEsNo`
  means the modem's constant LLR scale, 3 dB, or 10 dB for H_16200_9720).
  The two `M_*` columns are MATLAB's own decoders: `M_bp50` belief
  propagation and `M_nms50` normalised min-sum at its default scale 0.75,
  both at 50 iterations. Only those two columns cannot be reproduced in C.
- `llr_scale_<code>.csv`: FER with LLRs at the true Es/N0 (`true`) and at
  the modem's constant (`fixed`), table sum-product at 50 iterations, at
  each mode's real modulation (QPSK for the two QPSK codes, 16-QAM for
  H_16200_9720); 300 frames per point or 30 errors.
- `nms_tuning.txt`: MATLAB normalised min-sum (scale 0.70 to 0.90) and
  offset min-sum (offset 0.10 to 0.40) against MATLAB belief propagation,
  50 iterations, 400 frames per point; loss in dB at FER 10 % / 1 %. The C
  decoder has no offset min-sum, so this file has no C counterpart.

The figures in `docs/ldpc-figures/*.svg` were drawn from these files on
2026-10-09 and are kept as they were drawn.
