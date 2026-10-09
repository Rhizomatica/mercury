# MATLAB golden-model tests

Stage-by-stage tests of Mercury's modem against independent MATLAB standards
(Communications / DSP / Signal Processing Toolbox).  Plan and stage inventory:
`docs/MODEM-ANALYSIS.md`.

Run (MATLAB R2026a, from this directory or with it on the path):

```matlab
run_all            % everything
runtests('TestLdpc')
```

The C side is driven through small file-based tools in `utils/golden/`
(built automatically; `make -C utils/golden all export`).  Fixtures
(`fixtures/`) are generated, not committed.  Each performance test writes its
curves to `results/` and fails when the measured loss against the golden
exceeds the tolerance stated in the test, so a failure is a finding, not a
broken test.

Packages: `+c2` drives the C implementation; `+golden` holds the MATLAB
reference models and helpers.
