/*
 * Tests of the fldigi-rtty-core transmitter.
 *
 * Copyright (C) 2026 Gerolf Ziegenhain, DG6FL
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * usage: test_rtty_tx <golden_dir>
 *
 * 1. fldigi reference: the codes of each vector in <golden_dir>/tx/<name>.codes are
 *    rendered and flushed, scaled by fldigi's TX level (-12 dB) and rounded like
 *    libsndfile's PCM16 writer (lrint(x * 32767)); every sample must be within 1 LSB of
 *    fldigi 4.2.13's own TX output <name>.s16, and the length must be identical.
 * 2. Frame structure, stop-bit lengths, tone orientation, peak bound, clean end, invalid
 *    configurations, short output buffers and the version tag.
 */
#include "fldigi_rtty.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
            failures++;                                      \
        }                                                    \
    } while (0)

static const double PI = 3.14159265358979323846;

static frtty_tx *make(double rate, double mark, double space, double baud, double stop)
{
    frtty_tx_config c;
    c.sample_rate = rate;
    c.mark_hz = mark;
    c.space_hz = space;
    c.baud = baud;
    c.stop_bits = stop;
    return frtty_tx_create(&c);
}

/* Power of x[0..n) at frequency f (Goertzel). */
static double goertzel(const double *x, size_t n, double f, double rate)
{
    double w = 2.0 * PI * f / rate, coeff = 2.0 * cos(w), s1 = 0, s2 = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        double s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

/* 1 when the middle half of [start, start + len) is mark (the upper tone). */
static int bit_at(const double *x, size_t start, size_t len, double mark, double space,
                  double rate)
{
    size_t a = start + len / 4, n = len / 2;
    return goertzel(x + a, n, mark, rate) > goertzel(x + a, n, space, rate);
}

static void *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    void *buf;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) ((char *)buf)[n] = 0;
    *size = (size_t)n;
    return buf;
}

static void test_vector(const char *dir, const char *name)
{
    char path[1024];
    size_t size, ns, ncodes = 0, len, n, i;
    char *text, *p, *line;
    int16_t *ref;
    double rate, mark, space, baud, stop, mult, *out;
    uint8_t codes[4096];
    long maxdiff = 0;
    frtty_tx *tx;

    snprintf(path, sizeof(path), "%s/tx/%s.codes", dir, name);
    text = (char *)read_file(path, &size);
    CHECK(text != NULL, "cannot read %s", path);
    if (!text) return;
    p = text;
    while (*p == '#') p = strchr(p, '\n') + 1;
    CHECK(sscanf(p, "%lf %lf %lf %lf %lf", &rate, &mark, &space, &baud, &stop) == 5,
          "%s: bad config line", name);
    line = strchr(p, '\n') + 1;
    while (*line && *line != '\n') {
        long v = strtol(line, &p, 10);
        if (p == line) break;
        codes[ncodes++] = (uint8_t)v;
        line = p;
        while (*line == ' ') line++;
    }
    free(text);

    snprintf(path, sizeof(path), "%s/tx/%s.s16", dir, name);
    ref = (int16_t *)read_file(path, &size);
    CHECK(ref != NULL, "cannot read %s", path);
    if (!ref) return;
    ns = size / 2;

    tx = make(rate, mark, space, baud, stop);
    CHECK(tx != NULL, "%s: create failed", name);
    if (!tx) {
        free(ref);
        return;
    }
    len = ncodes * frtty_tx_frame_len(tx) + frtty_tx_flush_len(tx);
    out = (double *)malloc(len * sizeof(double));
    n = frtty_tx_render(tx, codes, ncodes, out, len);
    n += frtty_tx_flush(tx, out + n, len - n);
    CHECK(n == len && n == ns, "%s: %zu samples, fldigi %zu", name, n, ns);
    mult = pow(10, -12.0 / 20.0); /* modem::ModulateXmtr, modem.cxx:651-657 */
    for (i = 0; i < n && i < ns; i++) {
        long v = lrint(out[i] * mult * 32767.0);
        long d = labs(v - (long)ref[i]);
        if (d > maxdiff) maxdiff = d;
    }
    CHECK(maxdiff <= 1, "%s: max difference %ld LSB vs fldigi", name, maxdiff);
    printf("%-16s %zu codes %zu samples, max |diff| %ld LSB vs fldigi 4.2.13\n", name,
           ncodes, n, maxdiff);
    free(out);
    free(ref);
    frtty_tx_destroy(tx);
}

/* The symbol shaper is a raised-cosine step integrator centred in a table of
 * (int)(rate / baud * 5.49) samples (rtty.cxx:1557-1580), so every keying edge appears
 * table / 2 samples later on air: 483 samples at 8 kHz and 45.45 Bd. */
static size_t shaper_delay(double rate, double baud)
{
    return (size_t)((int)(rate / baud * 5.49) / 2);
}

static void test_frame(void)
{
    /* AC-770.1: one code at 8 kHz, 45.45 Bd, 1.5 stop bits, between two idles. */
    const double rate = 8000, mark = 2295, space = 2125;
    frtty_tx *tx = make(rate, mark, space, 45.45, 1.5);
    double out[4 * 1320];
    uint8_t codes[4] = {0x1F, 0x15, 0x1F, 0x1F}; /* LTRS, Y = 1 0 1 0 1 LSB first, LTRS */
    size_t n, b, d = shaper_delay(rate, 45.45), f0;
    CHECK(d == 483, "shaper delay %zu", d);
    CHECK(frtty_tx_frame_len(tx) == 1320, "frame length %zu", frtty_tx_frame_len(tx));
    CHECK(frtty_tx_flush_len(tx) == 1056, "flush length %zu", frtty_tx_flush_len(tx));
    n = frtty_tx_render(tx, codes, 4, out, 4 * 1320);
    CHECK(n == 4 * 1320, "rendered %zu", n);
    f0 = 1320 + d;
    CHECK(bit_at(out, f0, 176, mark, space, rate) == 0, "start bit is not space");
    for (b = 0; b < 5; b++)
        CHECK(bit_at(out, f0 + 176 * (b + 1), 176, mark, space, rate) == ((codes[1] >> b) & 1),
              "data bit %zu", b);
    CHECK(bit_at(out, f0 + 1056, 264, mark, space, rate) == 1, "stop element is not mark");
    frtty_tx_destroy(tx);
}

static void test_stop_lengths(void)
{
    /* AC-770.2 */
    const double stops[3] = {1.0, 1.5, 2.0};
    const size_t want[3] = {1232, 1320, 1408};
    int i;
    for (i = 0; i < 3; i++) {
        frtty_tx *tx = make(8000, 2295, 2125, 45.45, stops[i]);
        CHECK(tx && frtty_tx_frame_len(tx) == want[i], "stop %.1f: %zu", stops[i],
              tx ? frtty_tx_frame_len(tx) : 0);
        frtty_tx_destroy(tx);
    }
}

static void test_orientation(void)
{
    /* AC-770.3: LTRS idles; data bits at the mark, start bits at the space tone. */
    const double rate = 8000;
    frtty_tx *tx = make(rate, 2295, 2125, 45.45, 1.5);
    double *out = (double *)malloc(20 * 1320 * sizeof(double));
    double best_data = 0, best_start = 0, pd = -1, ps = -1, f;
    size_t d = shaper_delay(rate, 45.45);
    size_t n = frtty_tx_idle(tx, 20, out, 20 * 1320);
    CHECK(n == 20 * 1320, "idle rendered %zu", n);
    for (f = 2000; f <= 2400; f += 1) {
        double dp = 0, s = 0;
        size_t k;
        for (k = 2; k < 19; k++) {
            dp += goertzel(out + k * 1320 + d + 176, 5 * 176, f, rate);
            s += goertzel(out + k * 1320 + d + 44, 88, f, rate);
        }
        if (dp > pd) { pd = dp; best_data = f; }
        if (s > ps) { ps = s; best_start = f; }
    }
    CHECK(fabs(best_data - 2295) <= 5, "data-bit peak at %.0f Hz", best_data);
    CHECK(fabs(best_start - 2125) <= 5, "start-bit peak at %.0f Hz", best_start);
    free(out);
    frtty_tx_destroy(tx);
}

static void test_peak_and_end(void)
{
    /* AC-770.4 at 8 and 48 kHz. */
    const double rates[2] = {8000, 48000};
    uint8_t codes[64];
    int r;
    size_t i;
    for (i = 0; i < 64; i++) codes[i] = (uint8_t)((i * 7 + 3) & 0x1F);
    for (r = 0; r < 2; r++) {
        frtty_tx *tx = make(rates[r], 2295, 2125, 45.45, 1.5);
        size_t len = 64 * frtty_tx_frame_len(tx) + frtty_tx_flush_len(tx), n, tail;
        double *out = (double *)malloc(len * sizeof(double)), peak = 0, tail_peak = 0;
        n = frtty_tx_render(tx, codes, 64, out, len);
        n += frtty_tx_flush(tx, out + n, len - n);
        CHECK(n == len, "rate %.0f: %zu of %zu", rates[r], n, len);
        for (i = 0; i < n; i++)
            if (fabs(out[i]) > peak) peak = fabs(out[i]);
        tail = (size_t)(0.010 * rates[r]);
        for (i = n - tail; i < n; i++)
            if (fabs(out[i]) > tail_peak) tail_peak = fabs(out[i]);
        CHECK(peak <= 1.0, "rate %.0f: peak %.17g", rates[r], peak);
        CHECK(peak > 0.9, "rate %.0f: peak only %.3f", rates[r], peak);
        CHECK(tail_peak < 1e-3, "rate %.0f: last 10 ms at %.1f dBFS", rates[r],
              20 * log10(tail_peak + 1e-300));
        free(out);
        frtty_tx_destroy(tx);
    }
}

static void test_invalid(void)
{
    /* AC-770.6 and R-770.7 */
    CHECK(make(44100, 2295, 2125, 45.45, 1.5) == NULL, "rate 44100 accepted");
    CHECK(make(8000, 2295, 2285, 45.45, 1.5) == NULL, "shift 10 Hz accepted");
    CHECK(make(8000, 2125, 2295, 45.45, 1.5) == NULL, "mark below space accepted");
    CHECK(make(8000, 3100, 2930, 45.45, 1.5) == NULL, "mark above 3000 Hz accepted");
    CHECK(make(8000, 2295, 2125, 45.45, 1.25) == NULL, "stop 1.25 accepted");
    CHECK(make(8000, 2295, 2125, 0.0, 1.5) == NULL, "baud 0 accepted");
    {
        frtty_tx *wide = make(8000, 2295, 1295, 45.45, 1.5);
        CHECK(wide != NULL, "shift 1000 Hz refused");
        frtty_tx_destroy(wide);
    }
    CHECK(frtty_tx_create(NULL) == NULL, "NULL config accepted");
}

static void test_short_buffer(void)
{
    /* A too small buffer writes nothing and keeps the state. */
    frtty_tx *a = make(12000, 2295, 2125, 50, 1.5), *b = make(12000, 2295, 2125, 50, 1.5);
    size_t fl = frtty_tx_frame_len(a), i;
    double *x = (double *)malloc(2 * fl * sizeof(double));
    double *y = (double *)malloc(2 * fl * sizeof(double));
    uint8_t codes[2] = {0x01, 0x10};
    CHECK(frtty_tx_render(a, codes, 2, x, 2 * fl - 1) == 0, "short render wrote");
    CHECK(frtty_tx_idle(a, 2, x, fl) == 0, "short idle wrote");
    CHECK(frtty_tx_flush(a, x, frtty_tx_flush_len(a) - 1) == 0, "short flush wrote");
    CHECK(frtty_tx_render(a, codes, 2, x, 2 * fl) == 2 * fl, "render failed");
    CHECK(frtty_tx_render(b, codes, 2, y, 2 * fl) == 2 * fl, "render failed");
    for (i = 0; i < 2 * fl; i++)
        if (x[i] != y[i]) break;
    CHECK(i == 2 * fl, "state changed by a refused call (sample %zu)", i);
    /* Reset returns to the start state. */
    frtty_tx_reset(a);
    CHECK(frtty_tx_render(a, codes, 2, x, 2 * fl) == 2 * fl, "render after reset failed");
    for (i = 0; i < 2 * fl; i++)
        if (x[i] != y[i]) break;
    CHECK(i == 2 * fl, "reset differs at sample %zu", i);
    free(x);
    free(y);
    frtty_tx_destroy(a);
    frtty_tx_destroy(b);
}

static void test_version(void)
{
    const char *v = frtty_version();
    CHECK(strstr(v, "contract=rtty-c1") && strstr(v, "abi=1") && strstr(v, " tx=1"),
          "version %s", v);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "tests/golden";
    test_version();
    test_vector(dir, "rtty_cq");
    test_vector(dir, "rtty_cq_stop1");
    test_vector(dir, "rtty_cq_stop2");
    test_vector(dir, "rtty_50bd_long");
    test_frame();
    test_stop_lengths();
    test_orientation();
    test_peak_and_end();
    test_invalid();
    test_short_buffer();
    if (failures) {
        printf("%d TX check(s) FAILED\n", failures);
        return 1;
    }
    printf("all TX checks passed\n");
    return 0;
}
