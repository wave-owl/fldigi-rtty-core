/*
 * fldigi-rtty-core: RTTY (2-FSK) receiver after fldigi 4.2.13 rtty::rx_process.
 *
 * Copyright (C) 2026 Gerolf Ziegenhain, DG6FL
 * Derived from fldigi 4.2.13 (tag v4.2.13, commit e7c3ab37, mirror wave-owl/fldigi):
 * src/rtty/rtty.cxx and src/filters/fftfilt.cxx, Copyright (C) Dave Freese W1HKJ and
 * the fldigi developers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A C translation of wave-owl's Python port (src/modes/rtty/demod.py at b6be59b):
 *
 * 1. Two complex mixers move mark and space to 0 Hz (rtty.cxx:681-685, mixer at
 *    rtty.cxx:393-401).
 * 2. Each branch is low-pass filtered with fldigi's raised-cosine "RTTY filter"
 *    (fftfilt.cxx:245-290, K = 1.4, length 512 for 45.45 Bd, rtty.cxx:95-98, 221-229).
 * 3. Envelope and noise-floor trackers per branch and the "optimal ATC" (Kok Chen)
 *    mark/space decision (rtty.cxx:694-760).
 * 4. Start-bit search on the straddle of a mark/space edge, then the data bits and the
 *    stop bit sampled one symbol apart in the middle of a symbol-long bit buffer
 *    (rtty::rx, rtty.cxx:465-553).
 *
 * Mark is the upper tone in the audio (fldigi with USB and no reverse, rtty.cxx:648 and
 * rtty.cxx:845).
 */
#include "fldigi_rtty.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FRTTY_VERSION "0.1.0"

/* fldigi's shaping constant of the RTTY filter (fftfilt.cxx:263). */
#define RTTY_FILTER_K 1.4
/* FFT length of the RTTY filter for 45.45-110 Bd (rtty.cxx:98). */
#define FILTER_LENGTH 512

static const double PI = 3.14159265358979323846;
static const double TWO_PI = 2.0 * 3.14159265358979323846;

enum rx_state { RX_IDLE, RX_START, RX_DATA, RX_STOP };

typedef struct branch {
    double phase;
    double *hist;   /* last ntaps - 1 mixer outputs (re, im interleaved), oldest first */
    double *y;      /* filter output of the current block (re, im) */
} branch;

struct frtty_rx {
    frtty_config cfg;
    int symbollen;
    size_t ntaps;
    double *taps;
    size_t nfft;                    /* overlap-save FFT length */
    double *H;                      /* FFT of the zero-padded taps (re, im) */
    double *tw;                     /* twiddles e^{-2 pi j k / nfft}, k < nfft / 2 */
    double *work;                   /* nfft complex */
    double *zin;                    /* per-block mixer output (re, im) */
    branch mark, space;
    double offset_hz;
    double me, se, mn, sn;          /* envelopes and noise floors */
    uint8_t *buf;                   /* symbol-long bit buffer, circular */
    int buf_head;                   /* index of the oldest bit */
    int ones;
    enum rx_state state;
    int counter, bitcntr, rxdata;
    double last_re, last_im;        /* last mark filter output of the previous block */
    double sig, noise;
    long nchar;
};

const char *frtty_version(void)
{
    return "fldigi-rtty-core " FRTTY_VERSION " contract=" FRTTY_CONTRACT " abi=1";
}

/* sin(pi x) / (pi x), as fldigi sinc (src/include/misc.h:43-46) and numpy.sinc. */
static double sinc(double x)
{
    if (x == 0.0)
        return 1.0;
    return sin(PI * x) / (PI * x);
}

/*
 * FIR taps equal to fldigi's fftfilt::rtty_filter (fftfilt.cxx:245-290).
 * fldigi designs the response on an FFT grid of flen bins and applies it by overlap-add
 * with flen/2 input samples per block; the equivalent FIR is the inverse FFT of the same
 * response, flen/2 taps long and delayed by flen/4 samples.
 */
static void rtty_filter_taps(double baud, double rate, size_t flen, double *h)
{
    double f = RTTY_FILTER_K * baud / rate;
    size_t flen2 = flen / 2;
    double *sre = calloc(flen, sizeof(double));
    double *sim = calloc(flen, sizeof(double));
    size_t i, k;
    for (i = 0; i < flen2; i++) {
        double x = (double)i / (double)flen2;
        double c = cos(PI * x / (f * 4.0));
        double dht = x > 2.0 * f ? 0.0 : c * c;
        if (i == 0)
            dht = 1.0;
        dht = dht / sinc(2.0 * (double)i * f);
        sre[i] = dht * cos(-0.5 * PI * (double)i);
        sim[i] = dht * sin(-0.5 * PI * (double)i);
        sre[(flen - i) % flen] = dht * cos(0.5 * PI * (double)i);
        sim[(flen - i) % flen] = dht * sin(0.5 * PI * (double)i);
    }
    /* Real part of the inverse DFT, first flen/2 samples. */
    for (i = 0; i < flen2; i++) {
        double acc = 0.0;
        for (k = 0; k < flen; k++) {
            double a = TWO_PI * (double)((k * i) % flen) / (double)flen;
            acc += sre[k] * cos(a) - sim[k] * sin(a);
        }
        h[i] = acc / (double)flen;
    }
    free(sre);
    free(sim);
}

/* fldigi decayavg (src/include/misc.h:59-63). */
static double decayavg(double avg, double x, double weight)
{
    if (weight <= 1.0)
        return x;
    return avg + (x - avg) / weight;
}

/* Python float modulo (result has the sign of the divisor). */
static double pymod(double x, double y)
{
    double r = fmod(x, y);
    if (r != 0.0) {
        if ((r < 0.0) != (y < 0.0))
            r += y;
    } else {
        r = copysign(0.0, y);
    }
    return r;
}

/* In-place iterative radix-2 FFT of n complex values (re, im); inverse unscaled. */
static void fft(double *a, size_t n, const double *tw, int inverse)
{
    size_t i, j, len, k;
    for (i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = a[2 * i];
            a[2 * i] = a[2 * j];
            a[2 * j] = t;
            t = a[2 * i + 1];
            a[2 * i + 1] = a[2 * j + 1];
            a[2 * j + 1] = t;
        }
    }
    for (len = 2; len <= n; len <<= 1) {
        size_t half = len >> 1, step = n / len;
        for (i = 0; i < n; i += len) {
            for (k = 0; k < half; k++) {
                double wr = tw[2 * k * step];
                double wi = inverse ? -tw[2 * k * step + 1] : tw[2 * k * step + 1];
                double *u = a + 2 * (i + k), *v = a + 2 * (i + k + half);
                double xr = v[0] * wr - v[1] * wi, xi = v[0] * wi + v[1] * wr;
                v[0] = u[0] - xr;
                v[1] = u[1] - xi;
                u[0] += xr;
                u[1] += xi;
            }
        }
    }
}

frtty_rx *frtty_rx_create(const frtty_config *cfg)
{
    frtty_rx *rx;
    size_t sl, k;
    if (!cfg || !(cfg->sample_rate > 0.0) || !(cfg->baud > 0.0) || cfg->nbits < 1 ||
        cfg->nbits > 8 || !(cfg->afc_range_hz >= 0.0))
        return NULL;
    rx = calloc(1, sizeof(*rx));
    if (!rx)
        return NULL;
    rx->cfg = *cfg;
    /* rtty.cxx:256: symbol length in samples, rounded. */
    rx->symbollen = (int)(cfg->sample_rate / cfg->baud + 0.5);
    if (rx->symbollen < 2) {
        free(rx);
        return NULL;
    }
    sl = (size_t)rx->symbollen;
    rx->ntaps = FILTER_LENGTH / 2;
    for (rx->nfft = 2; rx->nfft < rx->ntaps - 1 + sl; rx->nfft <<= 1)
        ;
    rx->taps = malloc(rx->ntaps * sizeof(double));
    rx->H = malloc(2 * rx->nfft * sizeof(double));
    rx->tw = malloc(rx->nfft * sizeof(double));
    rx->work = malloc(2 * rx->nfft * sizeof(double));
    rx->zin = malloc(2 * sl * sizeof(double));
    rx->mark.hist = malloc(2 * (rx->ntaps - 1) * sizeof(double));
    rx->space.hist = malloc(2 * (rx->ntaps - 1) * sizeof(double));
    rx->mark.y = malloc(2 * sl * sizeof(double));
    rx->space.y = malloc(2 * sl * sizeof(double));
    rx->buf = malloc(sl);
    if (!rx->taps || !rx->H || !rx->tw || !rx->work || !rx->zin || !rx->mark.hist ||
        !rx->space.hist || !rx->mark.y || !rx->space.y || !rx->buf) {
        frtty_rx_destroy(rx);
        return NULL;
    }
    rtty_filter_taps(cfg->baud, cfg->sample_rate, FILTER_LENGTH, rx->taps);
    for (k = 0; k < rx->nfft / 2; k++) {
        double a = -TWO_PI * (double)k / (double)rx->nfft;
        rx->tw[2 * k] = cos(a);
        rx->tw[2 * k + 1] = sin(a);
    }
    memset(rx->H, 0, 2 * rx->nfft * sizeof(double));
    for (k = 0; k < rx->ntaps; k++)
        rx->H[2 * k] = rx->taps[k];
    fft(rx->H, rx->nfft, rx->tw, 0);
    frtty_rx_reset(rx);
    return rx;
}

void frtty_rx_destroy(frtty_rx *rx)
{
    if (!rx)
        return;
    free(rx->taps);
    free(rx->H);
    free(rx->tw);
    free(rx->work);
    free(rx->zin);
    free(rx->mark.hist);
    free(rx->space.hist);
    free(rx->mark.y);
    free(rx->space.y);
    free(rx->buf);
    free(rx);
}

void frtty_rx_reset(frtty_rx *rx)
{
    if (!rx)
        return;
    rx->offset_hz = 0.0;
    rx->mark.phase = rx->space.phase = 0.0;
    memset(rx->mark.hist, 0, 2 * (rx->ntaps - 1) * sizeof(double));
    memset(rx->space.hist, 0, 2 * (rx->ntaps - 1) * sizeof(double));
    rx->me = rx->se = rx->mn = rx->sn = 0.0;
    memset(rx->buf, 0, (size_t)rx->symbollen);
    rx->buf_head = 0;
    rx->ones = 0;
    rx->state = RX_IDLE;
    rx->counter = rx->bitcntr = rx->rxdata = 0;
    rx->last_re = rx->last_im = 0.0;
    rx->sig = rx->noise = 0.0;
    rx->nchar = 0;
}

void frtty_rx_set_tones(frtty_rx *rx, double mark_hz, double space_hz)
{
    if (!rx)
        return;
    rx->cfg.mark_hz = mark_hz;
    rx->cfg.space_hz = space_hz;
}

double frtty_rx_snr_db(const frtty_rx *rx)
{
    double ratio;
    if (!rx || rx->nchar == 0 || rx->noise <= 0.0)
        return 0.0;
    ratio = rx->sig / rx->noise;
    return ratio > 0.0 ? 10.0 * log10(ratio) : 0.0;
}

double frtty_rx_offset_hz(const frtty_rx *rx)
{
    return rx ? rx->offset_hz : 0.0;
}

size_t frtty_filter_len(const frtty_rx *rx)
{
    return rx ? rx->ntaps : 0;
}

const double *frtty_filter_taps(const frtty_rx *rx)
{
    return rx ? rx->taps : NULL;
}

/*
 * The RTTY filter on one block of mixer outputs (the reference: scipy lfilter with
 * persistent state), by overlap-save: [history | block | zeros] -> FFT -> * H -> IFFT;
 * the outputs at the block's positions are the linear convolution because
 * nfft >= ntaps - 1 + block length. Equal to the direct form up to rounding.
 */
static void filter_block(frtty_rx *rx, branch *b, const double *z, size_t len)
{
    size_t m = rx->ntaps - 1, N = rx->nfft, k;
    double *w = rx->work, scale = 1.0 / (double)N;
    memcpy(w, b->hist, 2 * m * sizeof(double));
    memcpy(w + 2 * m, z, 2 * len * sizeof(double));
    memset(w + 2 * (m + len), 0, 2 * (N - m - len) * sizeof(double));
    if (len >= m) {
        memcpy(b->hist, z + 2 * (len - m), 2 * m * sizeof(double));
    } else {
        memmove(b->hist, b->hist + 2 * len, 2 * (m - len) * sizeof(double));
        memcpy(b->hist + 2 * (m - len), z, 2 * len * sizeof(double));
    }
    fft(w, N, rx->tw, 0);
    for (k = 0; k < N; k++) {
        double hr = rx->H[2 * k], hi = rx->H[2 * k + 1];
        double xr = w[2 * k], xi = w[2 * k + 1];
        w[2 * k] = xr * hr - xi * hi;
        w[2 * k + 1] = xr * hi + xi * hr;
    }
    fft(w, N, rx->tw, 1);
    for (k = 0; k < len; k++) {
        b->y[2 * k] = w[2 * (m + k)] * scale;
        b->y[2 * k + 1] = w[2 * (m + k) + 1] * scale;
    }
}

/* fldigi rtty::rx (rtty.cxx:484-553) on the newest bit; a code or -1. */
static int rx_bit(frtty_rx *rx)
{
    int n = rx->symbollen;
    int mark;
#define BUF(i) rx->buf[(rx->buf_head + (i)) % n]
    if (rx->state == RX_IDLE) {
        /* is_mark_space (rtty.cxx:465-477): mark at the old end, space at the new end,
         * and the edge near the middle of the buffer. */
        if (BUF(0) && !BUF(n - 1)) {
            int correction = rx->ones;
            if (abs(n / 2 - correction) < 6) {
                rx->state = RX_START;
                rx->counter = correction;
            }
        }
        return -1;
    }
    rx->counter -= 1;
    if (rx->counter > 0)
        return -1;
    mark = BUF(n / 2);
#undef BUF
    if (rx->state == RX_START) {
        if (!mark) {
            rx->state = RX_DATA;
            rx->counter = n;
            rx->bitcntr = 0;
            rx->rxdata = 0;
        } else {
            rx->state = RX_IDLE;
        }
        return -1;
    }
    if (rx->state == RX_DATA) {
        rx->rxdata |= mark << rx->bitcntr;
        rx->bitcntr += 1;
        rx->counter = n;
        if (rx->bitcntr == rx->cfg.nbits)
            rx->state = RX_STOP;
        return -1;
    }
    /* stop bit */
    rx->state = RX_IDLE;
    return mark ? rx->rxdata : -1;
}

/*
 * Common frequency error from the mark branch's rotation over one sample.
 * fldigi measures the rotation of the mark (or space) filter output at each decoded
 * character and averages it with weight 8 (rtty.cxx:845-866); the stop bit is a mark,
 * so the mark branch is used.
 */
static void afc(frtty_rx *rx, double r1, double i1, double r0, double i0)
{
    double re, im, rot, ferr, v, lim = rx->cfg.afc_range_hz;
    if ((r0 == 0.0 && i0 == 0.0) || (r1 == 0.0 && i1 == 0.0))
        return;
    /* conj(z0) * z1 */
    re = r0 * r1 - (-i0) * i1;
    im = r0 * i1 + (-i0) * r1;
    rot = atan2(im, re);
    ferr = rot * rx->cfg.sample_rate / TWO_PI;
    if (fabs(ferr) > rx->cfg.baud / 2.0)
        return;
    v = decayavg(rx->offset_hz, rx->offset_hz + ferr, 8.0);
    rx->offset_hz = v > lim ? lim : (v < -lim ? -lim : v);
}

static void block(frtty_rx *rx, const double *x, size_t len, uint8_t *codes,
                  size_t max_codes, size_t *ncodes)
{
    int n = rx->symbollen;
    double att = n / 4.0, dec = n * 16.0, ndec = n * 48.0;
    double rate = rx->cfg.sample_rate;
    double mstep = TWO_PI * (rx->cfg.mark_hz + rx->offset_hz) / rate;
    double sstep = TWO_PI * (rx->cfg.space_hz + rx->offset_hz) / rate;
    double mph0 = rx->mark.phase, sph0 = rx->space.phase;
    double me = rx->me, se = rx->se, mn = rx->mn, sn = rx->sn;
    /* Current phasors (cos, sin) and the per-sample rotation e^{-j step}. */
    double mc_ = cos(mph0), ms_ = sin(mph0), sc_ = cos(sph0), ss_ = sin(sph0);
    double mrc = cos(mstep), mrs = sin(mstep), src = cos(sstep), srs = sin(sstep);
    const double *zm = rx->mark.y;
    size_t k;
    /* Mixers (rtty.cxx:681-685): e^{j(phase - step k)}; the phasor is rotated within one
     * symbol block and set exactly at every block start. */
    for (k = 0; k < len; k++) {
        double t = mc_ * mrc + ms_ * mrs;
        rx->zin[2 * k] = x[k] * mc_;
        rx->zin[2 * k + 1] = x[k] * ms_;
        ms_ = ms_ * mrc - mc_ * mrs;
        mc_ = t;
    }
    filter_block(rx, &rx->mark, rx->zin, len);
    for (k = 0; k < len; k++) {
        double t = sc_ * src + ss_ * srs;
        rx->zin[2 * k] = x[k] * sc_;
        rx->zin[2 * k + 1] = x[k] * ss_;
        ss_ = ss_ * src - sc_ * srs;
        sc_ = t;
    }
    filter_block(rx, &rx->space, rx->zin, len);
    for (k = 0; k < len; k++) {
        double mre, mim, sre, sim, m, s, floor, mc, sc, dm, ds, v3;
        int bit, code;
        mre = rx->mark.y[2 * k];
        mim = rx->mark.y[2 * k + 1];
        sre = rx->space.y[2 * k];
        sim = rx->space.y[2 * k + 1];
        m = sqrt(mre * mre + mim * mim);
        s = sqrt(sre * sre + sim * sim);
        /* Envelope and noise trackers (rtty.cxx:694-703). */
        me += (m - me) / (m > me ? att : dec);
        mn += (m - mn) / (m < mn ? att : ndec);
        se += (s - se) / (s > se ? att : dec);
        sn += (s - sn) / (s < sn ? att : ndec);
        floor = mn < sn ? mn : sn;
        /* Clipped magnitudes and the optimal ATC decision (rtty.cxx:705-760). */
        mc = m > me ? me : m;
        sc = s > se ? se : s;
        if (mc < floor)
            mc = floor;
        if (sc < floor)
            sc = floor;
        dm = me - floor;
        ds = se - floor;
        v3 = (mc - floor) * dm - (sc - floor) * ds - 0.25 * (dm * dm - ds * ds);
        bit = v3 > 0.0 ? 1 : 0;
        /* Symbol-long bit buffer (rtty.cxx:490-491). */
        rx->ones += bit - rx->buf[rx->buf_head];
        rx->buf[rx->buf_head] = (uint8_t)bit;
        rx->buf_head = (rx->buf_head + 1) % n;
        code = rx_bit(rx);
        if (code >= 0) {
            if (*ncodes < max_codes)
                codes[*ncodes] = (uint8_t)code;
            (*ncodes)++;
            rx->nchar++;
            rx->sig += (se > me ? se : me) - floor;
            rx->noise += floor > 0.0 ? floor : 1e-12;
            if (rx->cfg.afc) {
                if (k)
                    afc(rx, mre, mim, zm[2 * (k - 1)], zm[2 * (k - 1) + 1]);
                else
                    afc(rx, mre, mim, rx->last_re, rx->last_im);
            }
        }
    }
    rx->last_re = zm[2 * (len - 1)];
    rx->last_im = zm[2 * (len - 1) + 1];
    rx->mark.phase = pymod(mph0 - mstep * (double)len, TWO_PI);
    rx->space.phase = pymod(sph0 - sstep * (double)len, TWO_PI);
    rx->me = me;
    rx->se = se;
    rx->mn = mn;
    rx->sn = sn;
}

size_t frtty_rx_process(frtty_rx *rx, const double *x, size_t n, uint8_t *codes,
                        size_t max_codes)
{
    size_t start, ncodes = 0, sl;
    if (!rx || !x)
        return 0;
    sl = (size_t)rx->symbollen;
    /* Blocks of one symbol, so an AFC correction takes effect within a character. */
    for (start = 0; start < n; start += sl) {
        size_t len = n - start < sl ? n - start : sl;
        block(rx, x + start, len, codes, max_codes, &ncodes);
    }
    return ncodes;
}
