# Changelog

All notable changes to this project are documented here. Versioning and the meaning of
contract and ABI changes are described in the README, section "Contract and versioning".

## 0.1.1 (unreleased)

Documentation only; no change to the code, the contract (`rtty-c1`) or the ABI (1).

- README rewritten: scope, signal chain with fldigi 4.2.13 citations, configuration and
  API reference, C and Python usage, contract and versioning policy, verification,
  performance and memory, building, maintenance, roadmap.
- Comments in `src/rtty_rx.c` and `include/fldigi_rtty.h` now refer to the Python
  reference as the former one (wave-owl commit `b6be59b`), since it has been removed from
  wave-owl.
- Added this changelog.

Note: `FRTTY_VERSION` in `src/rtty_rx.c` still reads `0.1.0`; bump it when tagging 0.1.1.

## 0.1.0 (2026-10-16)

First release, contract `rtty-c1`, ABI 1.

- C99 port of wave-owl's Python port of the fldigi 4.2.13 RTTY receiver: mark/space
  mixers, raised-cosine RTTY filter (overlap-save FFT), envelope and noise trackers,
  optimal ATC, start-bit framing, optional AFC, SNR estimate.
- Codes identical to the Python reference on the golden data in `tests/golden/`; filter
  taps within 1e-12.
- Plain Makefile (static and shared library, out-of-tree builds), C test suite, CI on
  Ubuntu and macOS.
