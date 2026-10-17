/*
 * fldigi-rtty-core: RTTY (2-FSK) receiver after fldigi 4.2.13 rtty::rx_process.
 *
 * Copyright (C) 2026 Gerolf Ziegenhain, DG6FL
 * Derived from fldigi 4.2.13 (src/rtty/rtty.cxx, src/filters/fftfilt.cxx),
 * Copyright (C) Dave Freese W1HKJ and the fldigi developers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Transmitter after fldigi 4.2.13 rtty::send_char/send_idle/flush_stream (src/rtty_tx.c).
 *
 * Contract "rtty-c1", ABI 1, capability tag "tx=1". Receive: audio in (doubles), 5-bit
 * Baudot codes out. Transmit: 5-bit Baudot codes in, audio out. The Baudot text step
 * (LTRS/FIGS, unshift-on-space, CR/LF framing) is left to the caller in both directions.
 */
#ifndef FLDIGI_RTTY_H
#define FLDIGI_RTTY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRTTY_CONTRACT "rtty-c1"
#define FRTTY_ABI 1

typedef struct frtty_config {
    double sample_rate;   /* audio rate in Hz, e.g. 8000 */
    double mark_hz;       /* mark tone (upper tone for amateur RTTY on USB) */
    double space_hz;      /* space tone */
    double baud;          /* symbol rate, e.g. 45.45 */
    int nbits;            /* data bits per character, 5 for Baudot */
    int afc;              /* nonzero: track a common frequency error of both tones */
    double afc_range_hz;  /* maximum AFC pull, e.g. 50 */
} frtty_config;

typedef struct frtty_rx frtty_rx;

/* "fldigi-rtty-core <x.y.z> contract=rtty-c1 abi=1 tx=1" */
const char *frtty_version(void);

/* NULL on invalid configuration or out of memory. */
frtty_rx *frtty_rx_create(const frtty_config *cfg);
void frtty_rx_destroy(frtty_rx *rx);
void frtty_rx_reset(frtty_rx *rx);

/*
 * Process n samples; writes at most max_codes codes and returns the number of codes
 * received (codes beyond max_codes are counted but dropped). The input is processed in
 * blocks of one symbol starting at x[0], so an AFC correction takes effect within a
 * character; the result therefore depends on how the audio is split into calls when
 * AFC is on, exactly as in the former Python reference (wave-owl commit b6be59b).
 */
size_t frtty_rx_process(frtty_rx *rx, const double *x, size_t n,
                        uint8_t *codes, size_t max_codes);

/* Retune both tones; the receiver state is kept. */
void frtty_rx_set_tones(frtty_rx *rx, double mark_hz, double space_hz);

/* Envelope over noise floor of the stronger tone, averaged at decoded characters. */
double frtty_rx_snr_db(const frtty_rx *rx);
/* Current AFC offset in Hz (0 without AFC). */
double frtty_rx_offset_hz(const frtty_rx *rx);

/* The receive filter's taps (count = frtty_filter_len(), 256 for 45.45 Bd). */
size_t frtty_filter_len(const frtty_rx *rx);
const double *frtty_filter_taps(const frtty_rx *rx);

/*
 * ---- Transmitter (capability tag "tx=1", fldigi 4.2.13 shaped RTTY TX) ----
 *
 * Real AFSK audio in [-1, 1]: mark (the higher tone, 1 bits and the stop element) and
 * space (the lower tone, the start bit), each a phase-continuous oscillator keyed by a
 * raised-cosine symbol shaper, normalised by the running peak (rtty.cxx:915-1056).
 * A transmission is frtty_tx_render / frtty_tx_idle calls (any mix, the state carries
 * over) followed by one frtty_tx_flush; a later render starts the next transmission
 * from the decayed state (as fldigi does). frtty_tx_reset returns to the state of a
 * freshly started fldigi. One instance per thread; no global state.
 */
typedef struct frtty_tx_config {
    double sample_rate;   /* 8000, 12000 or 48000 */
    double mark_hz;       /* upper tone, 300..3000 Hz, e.g. 2295 */
    double space_hz;      /* lower tone, 300..3000 Hz, mark - space = 23..1000 Hz */
    double baud;          /* 45..300, e.g. 45.45 */
    double stop_bits;     /* 1.0, 1.5 or 2.0 */
} frtty_tx_config;

typedef struct frtty_tx frtty_tx;

/* NULL on an invalid configuration or out of memory. */
frtty_tx *frtty_tx_create(const frtty_tx_config *cfg);
void      frtty_tx_destroy(frtty_tx *tx);
/* Oscillator phases 0, shapers preset, maxamp 0 (fldigi restart + tx_init). */
void      frtty_tx_reset(frtty_tx *tx);

/* Samples of one character: 6 * symbollen + stoplen (1320 at 8 kHz, 45.45 Bd, 1.5). */
size_t frtty_tx_frame_len(const frtty_tx *tx);
/* Samples of the flush: 6 * symbollen (1056 at 8 kHz, 45.45 Bd). */
size_t frtty_tx_flush_len(const frtty_tx *tx);

/* Render n 5-bit codes (start, 5 data bits LSB first, stop); returns the samples
 * written, or 0 without a state change when out_max is too small. */
size_t frtty_tx_render(frtty_tx *tx, const uint8_t *codes, size_t n,
                       double *out, size_t out_max);
/* n LTRS idle characters (send_idle); same return rule. */
size_t frtty_tx_idle(frtty_tx *tx, size_t n, double *out, size_t out_max);
/* Shaped decay to silence (flush_stream); same return rule. */
size_t frtty_tx_flush(frtty_tx *tx, double *out, size_t out_max);

#ifdef __cplusplus
}
#endif

#endif
