# fldigi-rtty-core

A small C99 RTTY (2-FSK) receiver library: audio samples in, 5-bit Baudot codes out.

It is a C port of [wave-owl](https://github.com/wave-owl/wave-owl)'s Python port of the
fldigi 4.2.13 RTTY receiver:

| Part | fldigi 4.2.13 source |
|------|----------------------|
| Mark/space mixers, receive filters, optimal ATC (automatic threshold correction) | `src/rtty/rtty.cxx:681-760` |
| Character framing (start bit, data bits, stop bit) | `src/rtty/rtty.cxx:465-553` |
| AFC (common frequency error of both tones) | `src/rtty/rtty.cxx:845-866` |
| FFT filter (overlap-save) | `src/filters/fftfilt.cxx:245-290` |

The library returns exactly the code sequences of the Python reference on the golden data in
`tests/golden/`. It is used by wave-owl as its native RTTY core.

## Provenance

- Upstream: fldigi, <https://git.code.sf.net/p/fldigi/fldigi>, tag `v4.2.13`,
  commit `e7c3ab3709ad62a7d91829364eb48a05fa325c0d`
- Mirror: <https://github.com/wave-owl/fldigi>
- Python reference: wave-owl `src/modes/rtty/demod.py`

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). Derived from fldigi, Copyright (C) Dave Freese
W1HKJ and the fldigi developers.

## Contract

Contract `rtty-c1`, ABI 1 (`FRTTY_CONTRACT`, `FRTTY_ABI` in `include/fldigi_rtty.h`).
`frtty_version()` returns `"fldigi-rtty-core <x.y.z> contract=rtty-c1 abi=1"`; callers
check this string when loading the shared library.

## API

```c
#include "fldigi_rtty.h"

frtty_config cfg = {
    .sample_rate = 8000, .mark_hz = 2295, .space_hz = 2125,
    .baud = 45.45, .nbits = 5, .afc = 0, .afc_range_hz = 50,
};
frtty_rx *rx = frtty_rx_create(&cfg);           /* NULL on invalid config / OOM */
uint8_t codes[256];
size_t n = frtty_rx_process(rx, audio, nsamples, codes, 256);
frtty_rx_destroy(rx);
```

| Function | Purpose |
|----------|---------|
| `frtty_version()` | Version and contract string |
| `frtty_rx_create(cfg)` / `frtty_rx_destroy(rx)` | Create / free a receiver |
| `frtty_rx_reset(rx)` | Back to the initial state |
| `frtty_rx_process(rx, x, n, codes, max_codes)` | Process `n` samples (doubles); writes at most `max_codes` codes and returns the number received (excess codes are counted but dropped) |
| `frtty_rx_set_tones(rx, mark, space)` | Retune both tones, keeping the receiver state |
| `frtty_rx_snr_db(rx)` | Envelope over noise floor of the stronger tone, averaged at decoded characters |
| `frtty_rx_offset_hz(rx)` | Current AFC offset in Hz (0 without AFC) |
| `frtty_filter_len(rx)` / `frtty_filter_taps(rx)` | The receive filter taps (256 for 45.45 Bd) |

The input is processed in blocks of one symbol starting at `x[0]`. Without AFC the result
does not depend on how the audio is split into calls; with AFC it does (the correction takes
effect within a character), exactly as in the reference implementation.

Baudot/ITA2 to text conversion (LTRS/FIGS shifts, unshift-on-space) is left to the caller.

## Build

Plain `make`, C99, no dependencies besides `libm`:

```sh
make                          # static + shared library into build/
make test                     # build and run tests/test_rtty against tests/golden
make BUILD=/abs/dir shared    # out-of-tree shared library (used by wave-owl)
```

## Tests

`tests/test_rtty <golden_dir>` checks the golden cases (codes exact, SNR within 0.01 dB),
the filter taps (within 1e-12), the version string, chunk invariance without AFC,
`max_codes` truncation, invalid configurations, reset and `set_tones`.

The golden data (`tests/golden/`) is generated from a wave-owl checkout with
`tests/golden/make_golden.py /path/to/wave-owl`: raw little-endian `int16` (`.s16`, scaled
by 1/32768) or `float64` (`.f64`) audio at 8 kHz, `expected.txt`
(`name mark space afc chunk(0=none) snr_db file codes`) and `taps.txt`.

## Notes

- The receive filters are overlap-save FFT filters as in fldigi's `fftfilt`.
- Compiled with `-ffp-contract=off` (no fused multiply-add) so that the arithmetic follows
  the reference order; the codes are identical to the Python reference on the goldens.
- Performance: about 13.5 ms CPU for 18.9 s of audio at 8 kHz on an Apple M3 Pro, roughly
  10x faster than the Python reference.
