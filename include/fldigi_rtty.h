/*
 * fldigi-rtty-core: RTTY (2-FSK) receiver after fldigi 4.2.13 rtty::rx_process.
 *
 * Copyright (C) 2026 Gerolf Ziegenhain, DG6FL
 * Derived from fldigi 4.2.13 (src/rtty/rtty.cxx, src/filters/fftfilt.cxx),
 * Copyright (C) Dave Freese W1HKJ and the fldigi developers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Contract "rtty-c1", ABI 1. Audio in (doubles), 5-bit Baudot codes out; the Baudot
 * to text step (LTRS/FIGS, unshift-on-space) is left to the caller.
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

/* "fldigi-rtty-core <x.y.z> contract=rtty-c1 abi=1" */
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
 * AFC is on, exactly as in the reference implementation.
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

#ifdef __cplusplus
}
#endif

#endif
