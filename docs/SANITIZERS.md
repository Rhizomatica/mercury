# Sanitizers

Mercury can build under ThreadSanitizer or Address+UndefinedBehavior sanitizer
via two mutually-exclusive make toggles (default off):

    make SANITIZE_TSAN=1 -C tests test
    make SANITIZE_ASAN_UBSAN=1 -C tests test
    make SANITIZE_ASAN_UBSAN=1 && make integration-test

Runtime options are set via TSAN_OPTIONS / ASAN_OPTIONS / LSAN_OPTIONS /
UBSAN_OPTIONS (see .github/workflows/sanitizers.yml for the CI values).

## Suppressions

tests/sanitizer-suppressions/tsan.supp and lsan.supp hold ONLY known, triaged,
pre-existing findings. Every entry has a justification and, for real bugs, a
reference to where the fix is tracked. A NEW finding must fail the job, so do
not suppress broadly.

## CI status: informational -> required

The Sanitizers workflow starts as `continue-on-error: true` (informational)
because the baseline has pre-existing findings tracked elsewhere (notably the
arq_conn data race). Once those land and the baseline is clean, remove
`continue-on-error` from both jobs to make them required.
