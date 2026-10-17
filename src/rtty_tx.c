/*
 * fldigi-rtty-core: RTTY (2-FSK) transmitter after fldigi 4.2.13 rtty::tx_process.
 *
 * Copyright (C) 2026 Gerolf Ziegenhain, DG6FL
 * Derived from fldigi 4.2.13 (tag v4.2.13, commit e7c3ab37, mirror wave-owl/fldigi):
 * src/rtty/rtty.cxx and src/include/misc.h, Copyright (C) Dave Freese W1HKJ and the
 * fldigi developers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A line-by-line port of fldigi's shaped ("shaped_rtty", default on per
 * src/status/status.cxx:236) Baudot transmit path:
 *
 * 1. Bit and stop lengths in whole samples: symbollen = (int)(rate / baud + 0.5)
 *    (rtty.cxx:256), stoplen = (int)(stop * rate / baud + 0.5) (rtty.cxx:274-278).
 * 2. Two phase-continuous sine oscillators, one per tone (Oscillator::Update,
 *    rtty.cxx:1502-1508), each multiplied by its own SymbolShaper: six raised-cosine
 *    step integrators over a table of rate / baud * 5.49 samples (rtty.cxx:1510-1631,
 *    rcos from src/include/misc.h:82-88). Mark is freq1 = f + shift / 2, the upper tone
 *    (rtty.cxx:936-937); a 1 bit is mark.
 * 3. The sum is divided by the running peak maxamp; maxamp is pre-scanned once over 100
 *    alternating symbols of 3 bit lengths (rtty.cxx:942-955) and keeps growing with
 *    every larger sample (send_symbol rtty.cxx:915-980, send_stop 982-1030).
 * 4. A character is a start bit (space), 5 data bits LSB first and the stop element
 *    (send_char, rtty.cxx:1058-1100); an idle is one LTRS (send_idle, rtty.cxx:1102-1109);
 *    the end of a transmission is 6 bit lengths with both shapers released to zero
 *    (flush_stream, rtty.cxx:1032-1056).
 *
 * Deliberate differences from fldigi (wave-owl D-770):
 * - maxamp is per instance, not fldigi's process-wide global (rtty.cxx:891). Each
 *   frtty_tx_create/frtty_tx_reset starts like a freshly started fldigi (maxamp 0,
 *   oscillator phases 0, shapers preset), so the first frame matches fldigi's first
 *   transmission after start-up.
 * - The shaper counters start free at any table size (see shaper_reset); fldigi's start
 *   value 1024 is kept where it already means "free" (8000 Hz).
 * - Only shaped keying: no unshaped nco() (rtty.cxx:893-900), no PseudoFSK FSKnco()
 *   (rtty.cxx:902-909), no reverse, no parity, no 7/8-bit ASCII.
 * - The Baudot text framing (tx_process, baudot_enc) stays with the caller: this file
 *   takes 5-bit codes only.
 */
#include "fldigi_rtty.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const double PI = 3.14159265358979323846;

/* TWOPI, fldigi src/include/modem.h:45. */
#define TWOPI (2.0 * PI)
/* Data bits of a Baudot character (rtty.cxx:240, BITS[0]). */
#define NBITS 5
/* LTRS as sent on the line (rtty.cxx:1067-1068). */
#define CODE_LTRS 0x1F
/* Flush length in bit lengths (rtty.cxx:1038). */
#define FLUSH_SYMBOLS 6
/* maxamp pre-scan: 100 alternating symbols of 3 bit lengths each (rtty.cxx:942-955). */
#define PRESCAN_SYMBOLS 100
#define PRESCAN_LENGTHS 3

/* class SymbolShaper, src/include/rtty.h:70-104 and rtty.cxx:1516-1626. */
typedef struct shaper {
    int table_size;
    double *table;
    int state;
    double accumulator;
    long counter[6];
    double factor[6];
} shaper;

struct frtty_tx {
    frtty_tx_config cfg;
    double freq1;          /* mark, f + shift / 2 (rtty.cxx:936) */
    double freq2;          /* space, f - shift / 2 (rtty.cxx:937) */
    int symbollen;
    int stoplen;
    double maxamp;         /* fldigi global rtty.cxx:891, here per instance */
    double phase1;         /* Oscillator m_phase of m_Osc1 (mark) */
    double phase2;         /* Oscillator m_phase of m_Osc2 (space) */
    shaper sh1;            /* m_SymShaper1 (mark) */
    shaper sh2;            /* m_SymShaper2 (space) */
};

/* rcos, src/include/misc.h:82-88. */
static double rcos(double t, double T, double alpha)
{
    double taT;
    if (t == 0) return 1.0;
    taT = T / (2.0 * alpha);
    if (fabs(t) == taT) return ((alpha / 2.0) * sin(PI / (2.0 * alpha)));
    return (sin(PI * t / T) / (PI * t / T)) * cos(alpha * PI * t / T) /
           (1.0 - (t / taT) * (t / taT));
}

/* SymbolShaper::reset, rtty.cxx:1531-1547. fldigi starts the six counters at 1024,
 * which marks an integrator as free only while the table has at most 1024 entries; that
 * holds for every fldigi baud rate at fldigi's 8000 Hz (966 entries at 45.45 Bd). At 12
 * and 48 kHz the table is longer and fldigi's value would leave the integrators busy and
 * drop the first transitions, so the counters start at the table size: free, and
 * sample-identical to fldigi wherever fldigi itself works. */
static void shaper_reset(shaper *s)
{
    int i;
    s->state = 0;
    s->accumulator = 0.0;
    for (i = 0; i < 6; i++) {
        s->counter[i] = s->table_size > 1024 ? s->table_size : 1024;
        s->factor[i] = 0.0;
    }
}

/* SymbolShaper::Preset, rtty.cxx:1549-1594; table size truncated to int as in fldigi
 * (int m_table_size, rtty.h:81), integral normalised in long double (rtty.cxx:1567). */
static int shaper_preset(shaper *s, double baud, double sr)
{
    double baud_rate = baud;
    double sample_rate = sr;
    long double sum = 0.0;
    int x;

    /* The size depends only on the configuration, so one allocation per instance. */
    s->table_size = (int)(sample_rate / baud_rate * 5.49);
    if (!s->table)
        s->table = (double *)malloc((size_t)s->table_size * sizeof(double));
    if (!s->table) return 0;
    for (x = 0; x < s->table_size; ++x) {
        int const offset = s->table_size / 2;
        double wfactor = 1.0 / 1.568; /* optimal */
        double const T = wfactor * sample_rate / (baud_rate * 2.0);
        double const t = (x - offset);
        s->table[x] = rcos(t, T, 1.0);
        sum += s->table[x];
    }
    for (x = 0; x < s->table_size; ++x)
        s->table[x] *= 1.0 / sum;
    shaper_reset(s);
    return 1;
}

/* SymbolShaper::Update, rtty.cxx:1596-1626. */
static double shaper_update(shaper *s, int state)
{
    int i;
    if (s->state != state) {
        s->state = state;
        for (i = 0; i < 6; i++) {
            if (s->counter[i] >= s->table_size) {
                s->counter[i] = 0;
                s->factor[i] = state ? +1.0 : -1.0;
                break;
            }
        }
    }
    for (i = 0; i < 6; i++)
        if (s->counter[i] < s->table_size)
            s->accumulator += s->factor[i] * s->table[s->counter[i]++];
    return (s->accumulator / sqrt(2));
}

/* Oscillator::Update, rtty.cxx:1502-1508. */
static double osc_update(double *phase, double frequency, double samplerate)
{
    *phase += frequency / samplerate * TWOPI;
    if (*phase > TWOPI) *phase -= TWOPI;
    return (sin(*phase));
}

/* One sample of the shaped two-tone signal (rtty.cxx:957-959, 1010-1012, 1040-1042). */
static double tone_sample(frtty_tx *tx, int symbol)
{
    double rate = tx->cfg.sample_rate;
    double mark = shaper_update(&tx->sh1, symbol) * osc_update(&tx->phase1, tx->freq1, rate);
    double space = shaper_update(&tx->sh2, !symbol) * osc_update(&tx->phase2, tx->freq2, rate);
    return mark + space;
}

/* The normalisation shared by send_symbol, send_stop and flush_stream
 * (rtty.cxx:961-965, 1014-1016, 1044-1046). */
static double normalised(frtty_tx *tx, double signal)
{
    if (tx->maxamp < fabs(signal)) tx->maxamp = fabs(signal);
    return tx->maxamp ? (signal / tx->maxamp) : 0.0;
}

/* rtty::send_symbol, shaped branch with the maxamp pre-scan (rtty.cxx:915-980). */
static double *send_symbol(frtty_tx *tx, int symbol, int len, double *out)
{
    int i, j;
    if (tx->maxamp == 0) {
        int sym = 0;
        for (j = 0; j < PRESCAN_SYMBOLS; j++) {
            sym = sym ? 0 : 1;
            for (i = 0; i < PRESCAN_LENGTHS * len; ++i) {
                double signal = tone_sample(tx, sym);
                if (tx->maxamp < fabs(signal)) tx->maxamp = fabs(signal);
            }
        }
    }
    for (i = 0; i < len; ++i)
        *out++ = normalised(tx, tone_sample(tx, symbol));
    return out;
}

/* rtty::send_stop, shaped branch (rtty.cxx:982-1030): mark for stoplen samples. */
static double *send_stop(frtty_tx *tx, double *out)
{
    int i;
    for (i = 0; i < tx->stoplen; ++i)
        *out++ = normalised(tx, tone_sample(tx, 1));
    return out;
}

/* rtty::send_char for nbits == 5 without parity (rtty.cxx:1058-1100). */
static double *send_char(frtty_tx *tx, int c, double *out)
{
    int i;
    out = send_symbol(tx, 0, tx->symbollen, out);              /* start bit */
    for (i = 0; i < NBITS; i++)
        out = send_symbol(tx, (c >> i) & 1, tx->symbollen, out); /* data bits */
    return send_stop(tx, out);                                  /* stop bit(s) */
}

static int valid_config(const frtty_tx_config *c)
{
    double shift;
    if (!c) return 0;
    if (c->sample_rate != 8000.0 && c->sample_rate != 12000.0 && c->sample_rate != 48000.0)
        return 0;
    if (!(c->mark_hz >= 300.0 && c->mark_hz <= 3000.0)) return 0;
    if (!(c->space_hz >= 300.0 && c->space_hz <= 3000.0)) return 0;
    shift = c->mark_hz - c->space_hz;
    if (!(shift >= 23.0 && shift <= 1000.0)) return 0;
    /* fldigi's baud table spans 45..300 Bd (rtty.cxx:97). */
    if (!(c->baud >= 45.0 && c->baud <= 300.0)) return 0;
    if (c->stop_bits != 1.0 && c->stop_bits != 1.5 && c->stop_bits != 2.0) return 0;
    return 1;
}

frtty_tx *frtty_tx_create(const frtty_tx_config *cfg)
{
    frtty_tx *tx;
    double f, shift;
    if (!valid_config(cfg)) return NULL;
    tx = (frtty_tx *)calloc(1, sizeof(*tx));
    if (!tx) return NULL;
    tx->cfg = *cfg;
    /* fldigi keys around the carrier f with the shift (rtty.cxx:936-937). */
    f = (cfg->mark_hz + cfg->space_hz) / 2.0;
    shift = cfg->mark_hz - cfg->space_hz;
    tx->freq1 = f + shift / 2.0;
    tx->freq2 = f - shift / 2.0;
    tx->symbollen = (int)(cfg->sample_rate / cfg->baud + 0.5);          /* rtty.cxx:256 */
    tx->stoplen = (int)(cfg->stop_bits * cfg->sample_rate / cfg->baud + 0.5); /* :278 */
    if (!shaper_preset(&tx->sh1, cfg->baud, cfg->sample_rate) ||
        !shaper_preset(&tx->sh2, cfg->baud, cfg->sample_rate)) {
        frtty_tx_destroy(tx);
        return NULL;
    }
    frtty_tx_reset(tx);
    return tx;
}

void frtty_tx_destroy(frtty_tx *tx)
{
    if (!tx) return;
    free(tx->sh1.table);
    free(tx->sh2.table);
    free(tx);
}

/* restart (rtty.cxx:302-303, 1588-1593: shapers preset, maxamp 0) and tx_init
 * (rtty.cxx:103-113); the oscillators start at phase 0 as after construction
 * (rtty.cxx:1494-1500). */
void frtty_tx_reset(frtty_tx *tx)
{
    if (!tx) return;
    tx->phase1 = 0.0;
    tx->phase2 = 0.0;
    shaper_preset(&tx->sh1, tx->cfg.baud, tx->cfg.sample_rate);
    shaper_preset(&tx->sh2, tx->cfg.baud, tx->cfg.sample_rate);
    tx->maxamp = 0;
}

size_t frtty_tx_frame_len(const frtty_tx *tx)
{
    return tx ? (size_t)((1 + NBITS) * tx->symbollen + tx->stoplen) : 0;
}

size_t frtty_tx_flush_len(const frtty_tx *tx)
{
    return tx ? (size_t)(FLUSH_SYMBOLS * tx->symbollen) : 0;
}

size_t frtty_tx_render(frtty_tx *tx, const uint8_t *codes, size_t n,
                       double *out, size_t out_max)
{
    size_t i, need;
    double *p = out;
    if (!tx || (n && (!codes || !out))) return 0;
    need = n * frtty_tx_frame_len(tx);
    if (need > out_max) return 0;
    for (i = 0; i < n; i++)
        p = send_char(tx, codes[i] & 0x1F, p);
    return need;
}

/* rtty::send_idle for nbits == 5: one LTRS (rtty.cxx:1102-1109). */
size_t frtty_tx_idle(frtty_tx *tx, size_t n, double *out, size_t out_max)
{
    size_t i, need;
    double *p = out;
    if (!tx || (n && !out)) return 0;
    need = n * frtty_tx_frame_len(tx);
    if (need > out_max) return 0;
    for (i = 0; i < n; i++)
        p = send_char(tx, CODE_LTRS, p);
    return need;
}

/* rtty::flush_stream (rtty.cxx:1032-1056): both shapers released for 6 bit lengths. */
size_t frtty_tx_flush(frtty_tx *tx, double *out, size_t out_max)
{
    size_t i, need;
    double rate;
    if (!tx || !out) return 0;
    need = frtty_tx_flush_len(tx);
    if (need > out_max) return 0;
    rate = tx->cfg.sample_rate;
    for (i = 0; i < need; ++i) {
        double mark = shaper_update(&tx->sh1, 0) * osc_update(&tx->phase1, tx->freq1, rate);
        double space = shaper_update(&tx->sh2, 0) * osc_update(&tx->phase2, tx->freq2, rate);
        out[i] = normalised(tx, mark + space);
    }
    return need;
}
