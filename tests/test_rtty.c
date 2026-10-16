/*
 * Tests of fldigi-rtty-core against the golden data in tests/golden (see make_golden.py).
 * Usage: test_rtty <golden_dir>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "fldigi_rtty.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CASES 32
#define MAX_CODES 4096
#define RATE 8000.0
#define BAUD 45.45

typedef struct {
    char name[64];
    double mark, space, snr_db;
    int afc;
    size_t chunk;
    char file[128];
    uint8_t codes[MAX_CODES];
    size_t ncodes;
} golden_case;

static const char *g_dir;
static int g_failures;

static void report(const char *test, int ok, const char *detail)
{
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", test, detail[0] ? ": " : "", detail);
    if (!ok)
        g_failures++;
}

static void path_join(char *out, size_t size, const char *file)
{
    snprintf(out, size, "%s/%s", g_dir, file);
}

/* Read a whole file; returns NULL on error. */
static unsigned char *read_file(const char *file, size_t *len)
{
    char path[1024];
    FILE *f;
    unsigned char *buf;
    long size;

    path_join(path, sizeof path, file);
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc(size > 0 ? (size_t)size : 1);
    if (buf && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = (size_t)size;
    return buf;
}

/* Load .s16 (little-endian int16, scaled by 1/32768) or .f64 (little-endian float64). */
static double *load_audio(const char *file, size_t *n)
{
    size_t len, i, dot = strlen(file);
    unsigned char *raw = read_file(file, &len);
    double *x = NULL;

    if (!raw)
        return NULL;
    if (dot > 4 && strcmp(file + dot - 4, ".s16") == 0) {
        *n = len / 2;
        x = malloc(*n * sizeof *x + 1);
        for (i = 0; x && i < *n; i++) {
            int16_t v = (int16_t)(uint16_t)(raw[2 * i] | (raw[2 * i + 1] << 8));
            x[i] = v / 32768.0;
        }
    } else if (dot > 4 && strcmp(file + dot - 4, ".f64") == 0) {
        *n = len / 8;
        x = malloc(*n * sizeof *x + 1);
        for (i = 0; x && i < *n; i++) {
            uint64_t bits = 0;
            int b;
            for (b = 7; b >= 0; b--)
                bits = (bits << 8) | raw[8 * i + (size_t)b];
            memcpy(&x[i], &bits, sizeof bits);
        }
    }
    free(raw);
    return x;
}

static int load_expected(golden_case *cases, size_t *ncases)
{
    char path[1024], codes[16384];
    FILE *f;

    path_join(path, sizeof path, "expected.txt");
    f = fopen(path, "r");
    if (!f)
        return 0;
    *ncases = 0;
    while (*ncases < MAX_CASES) {
        golden_case *c = &cases[*ncases];
        unsigned long chunk;
        char *tok;
        if (fscanf(f, "%63s %lf %lf %d %lu %lf %127s %16383s", c->name, &c->mark, &c->space,
                   &c->afc, &chunk, &c->snr_db, c->file, codes) != 8)
            break;
        c->chunk = (size_t)chunk;
        c->ncodes = 0;
        if (strcmp(codes, "-") != 0)
            for (tok = strtok(codes, ","); tok && c->ncodes < MAX_CODES; tok = strtok(NULL, ","))
                c->codes[c->ncodes++] = (uint8_t)atoi(tok);
        (*ncases)++;
    }
    fclose(f);
    return *ncases > 0;
}

static frtty_config make_config(double mark, double space, int afc)
{
    frtty_config cfg;
    cfg.sample_rate = RATE;
    cfg.mark_hz = mark;
    cfg.space_hz = space;
    cfg.baud = BAUD;
    cfg.nbits = 5;
    cfg.afc = afc;
    cfg.afc_range_hz = 50.0;
    return cfg;
}

/* Feed x in calls of `chunk` samples (0 = one call); returns the number of codes. */
static size_t run(frtty_rx *rx, const double *x, size_t n, size_t chunk, uint8_t *codes)
{
    size_t got = 0, i;

    if (chunk == 0)
        return frtty_rx_process(rx, x, n, codes, MAX_CODES);
    for (i = 0; i < n; i += chunk) {
        size_t len = n - i < chunk ? n - i : chunk;
        got += frtty_rx_process(rx, x + i, len, codes + got, MAX_CODES - got);
    }
    return got;
}

static int same_codes(const uint8_t *a, size_t na, const uint8_t *b, size_t nb)
{
    return na == nb && (na == 0 || memcmp(a, b, na) == 0);
}

static void test_golden(const golden_case *c)
{
    char test[128], detail[256];
    uint8_t codes[MAX_CODES];
    size_t n, got;
    double *x = load_audio(c->file, &n);
    frtty_config cfg = make_config(c->mark, c->space, c->afc);
    frtty_rx *rx;
    double snr;

    snprintf(test, sizeof test, "golden %s", c->name);
    if (!x) {
        report(test, 0, "cannot load audio");
        return;
    }
    rx = frtty_rx_create(&cfg);
    if (!rx) {
        report(test, 0, "create failed");
        free(x);
        return;
    }
    got = run(rx, x, n, c->chunk, codes);
    snr = frtty_rx_snr_db(rx);
    snprintf(detail, sizeof detail, "%lu codes (expected %lu), snr %.4f dB (expected %.4f)",
             (unsigned long)got, (unsigned long)c->ncodes, snr, c->snr_db);
    report(test, same_codes(codes, got, c->codes, c->ncodes) && fabs(snr - c->snr_db) <= 0.01,
           detail);
    frtty_rx_destroy(rx);
    free(x);
}

static void test_taps(void)
{
    char path[1024], detail[128];
    frtty_config cfg = make_config(1585.0, 1415.0, 0);
    frtty_rx *rx = frtty_rx_create(&cfg);
    const double *taps;
    double ref, maxerr = 0.0;
    size_t i, len;
    int ok;
    FILE *f;

    path_join(path, sizeof path, "taps.txt");
    f = fopen(path, "r");
    if (!rx || !f) {
        report("filter taps", 0, "setup failed");
        if (f)
            fclose(f);
        frtty_rx_destroy(rx);
        return;
    }
    len = frtty_filter_len(rx);
    taps = frtty_filter_taps(rx);
    ok = len == 256 && taps != NULL;
    for (i = 0; ok && i < len; i++) {
        if (fscanf(f, "%lf", &ref) != 1) {
            ok = 0;
            break;
        }
        if (fabs(taps[i] - ref) > maxerr)
            maxerr = fabs(taps[i] - ref);
    }
    ok = ok && maxerr <= 1e-12 && fscanf(f, "%lf", &ref) != 1;
    snprintf(detail, sizeof detail, "%lu taps, max error %.3g", (unsigned long)len, maxerr);
    report("filter taps", ok, detail);
    fclose(f);
    frtty_rx_destroy(rx);
}

static void test_version(void)
{
    const char *v = frtty_version();
    report("version", v && strstr(v, "contract=rtty-c1 abi=1") != NULL, v ? v : "NULL");
}

static void test_chunk_invariance(const golden_case *clean)
{
    static const size_t chunks[] = {1, 7, 333, 1000};
    uint8_t ref[MAX_CODES], codes[MAX_CODES];
    size_t n, nref, got, k;
    double *x = load_audio(clean->file, &n);
    frtty_config cfg = make_config(clean->mark, clean->space, 0);
    frtty_rx *rx = frtty_rx_create(&cfg);
    char test[64], detail[64];

    if (!x || !rx) {
        report("chunk invariance", 0, "setup failed");
        free(x);
        frtty_rx_destroy(rx);
        return;
    }
    nref = run(rx, x, n, 0, ref);
    for (k = 0; k < sizeof chunks / sizeof chunks[0]; k++) {
        frtty_rx_reset(rx);
        got = run(rx, x, n, chunks[k], codes);
        snprintf(test, sizeof test, "chunk invariance %lu", (unsigned long)chunks[k]);
        snprintf(detail, sizeof detail, "%lu codes (one call: %lu)",
                 (unsigned long)got, (unsigned long)nref);
        report(test, nref > 0 && same_codes(codes, got, ref, nref), detail);
    }
    frtty_rx_destroy(rx);
    free(x);
}

static void test_truncation(const golden_case *clean)
{
    uint8_t codes[4];
    size_t n, got;
    double *x = load_audio(clean->file, &n);
    frtty_config cfg = make_config(clean->mark, clean->space, 0);
    frtty_rx *rx = frtty_rx_create(&cfg);
    char detail[96];

    if (!x || !rx) {
        report("max_codes truncation", 0, "setup failed");
        free(x);
        frtty_rx_destroy(rx);
        return;
    }
    got = frtty_rx_process(rx, x, n, codes, 4);
    snprintf(detail, sizeof detail, "returned %lu (expected %lu)",
             (unsigned long)got, (unsigned long)clean->ncodes);
    report("max_codes truncation",
           got == clean->ncodes && memcmp(codes, clean->codes, 4) == 0, detail);
    frtty_rx_reset(rx);
    got = frtty_rx_process(rx, x, n, NULL, 0);
    report("max_codes zero", got == clean->ncodes, "");
    frtty_rx_destroy(rx);
    free(x);
}

static void test_invalid_config(void)
{
    frtty_config cfg = make_config(1585.0, 1415.0, 0);
    frtty_rx *rx;

    cfg.baud = 0.0;
    rx = frtty_rx_create(&cfg);
    report("invalid config baud 0", rx == NULL, "");
    frtty_rx_destroy(rx);

    cfg = make_config(1585.0, 1415.0, 0);
    cfg.nbits = 0;
    rx = frtty_rx_create(&cfg);
    report("invalid config nbits 0", rx == NULL, "");
    frtty_rx_destroy(rx);

    report("create NULL config", frtty_rx_create(NULL) == NULL, "");
}

static void test_reset(const golden_case *c)
{
    uint8_t first[MAX_CODES], second[MAX_CODES];
    size_t n, n1, n2;
    double *x = load_audio(c->file, &n);
    frtty_config cfg = make_config(c->mark, c->space, c->afc);
    frtty_rx *rx = frtty_rx_create(&cfg);
    double snr1, snr2;
    char test[96];

    snprintf(test, sizeof test, "reset %s", c->name);
    if (!x || !rx) {
        report(test, 0, "setup failed");
        free(x);
        frtty_rx_destroy(rx);
        return;
    }
    n1 = run(rx, x, n, c->chunk, first);
    snr1 = frtty_rx_snr_db(rx);
    frtty_rx_reset(rx);
    n2 = run(rx, x, n, c->chunk, second);
    snr2 = frtty_rx_snr_db(rx);
    report(test, same_codes(first, n1, second, n2) && snr1 == snr2, "");
    frtty_rx_destroy(rx);
    free(x);
}

static void test_set_tones(const golden_case *clean)
{
    uint8_t codes[MAX_CODES];
    size_t n, got;
    double *x = load_audio(clean->file, &n);
    frtty_config cfg = make_config(2295.0, 2125.0, 0);
    frtty_rx *rx = frtty_rx_create(&cfg);
    char detail[64];

    if (!x || !rx) {
        report("set_tones", 0, "setup failed");
        free(x);
        frtty_rx_destroy(rx);
        return;
    }
    frtty_rx_set_tones(rx, clean->mark, clean->space);
    got = run(rx, x, n, 0, codes);
    snprintf(detail, sizeof detail, "%lu codes (expected %lu)",
             (unsigned long)got, (unsigned long)clean->ncodes);
    report("set_tones", same_codes(codes, got, clean->codes, clean->ncodes), detail);
    frtty_rx_destroy(rx);
    free(x);
}

static const golden_case *find_case(const golden_case *cases, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (strcmp(cases[i].name, name) == 0)
            return &cases[i];
    return NULL;
}

int main(int argc, char **argv)
{
    static golden_case cases[MAX_CASES];
    size_t ncases = 0, i;
    const golden_case *clean, *afc_chunk;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <golden_dir>\n", argv[0]);
        return 2;
    }
    g_dir = argv[1];
    if (!load_expected(cases, &ncases)) {
        fprintf(stderr, "cannot read %s/expected.txt\n", g_dir);
        return 2;
    }
    clean = find_case(cases, ncases, "rtty_cq_clean");
    afc_chunk = find_case(cases, ncases, "rtty_cq_afc20_chunk1000");
    if (!clean || !afc_chunk) {
        fprintf(stderr, "golden cases rtty_cq_clean / rtty_cq_afc20_chunk1000 missing\n");
        return 2;
    }

    test_version();
    test_taps();
    for (i = 0; i < ncases; i++)
        test_golden(&cases[i]);
    test_chunk_invariance(clean);
    test_truncation(clean);
    test_invalid_config();
    test_reset(clean);
    test_reset(afc_chunk);
    test_set_tones(clean);

    printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
