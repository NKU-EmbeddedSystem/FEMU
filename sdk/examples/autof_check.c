/*
 * autof_check.c -- M3 auto-f acceptance tool: run N search batches with
 * either a fixed collab split (-F 0.25) or the max-model auto split
 * (-F auto), reporting per-batch wall / engine counters / f_cur, and
 * writing the experiment dump format for the byte gate.
 */
#include "cylon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    const char *blob = NULL, *qfile = NULL, *dumpfile = NULL;
    uint32_t nq = 999, batch = 333, k = 10, ef = 100;
    int auto_f = 0;
    double fixed_f = 0.5;
    double last_f = 0.5;
    cylon_config cfg;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) blob = argv[++i];
        else if (!strcmp(argv[i], "-q")) qfile = argv[++i];
        else if (!strcmp(argv[i], "-o")) dumpfile = argv[++i];
        else if (!strcmp(argv[i], "-n")) nq = strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-b")) batch = strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-k")) k = strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-e")) ef = strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-F")) {
            if (!strcmp(argv[i + 1], "auto")) { auto_f = 1; i++; }
            else { fixed_f = strtod(argv[++i], NULL); }
        }
        else { fprintf(stderr, "bad arg %s\n", argv[i]); return 1; }
    }
    if (!blob || !qfile || !dumpfile) {
        fprintf(stderr, "usage: autof_check -f blob -q q_fp16 -o dump "
                "[-n 999 -b 333 -k 10 -e 100] [-F auto|0.25|0.5|0.65]\n");
        return 1;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.blob_path = blob;
    cfg.cpu_frac = auto_f ? NAN : fixed_f;
    cfg.ef = ef;
    cfg.notify = CYLON_NOTIFY_POLL;
    cylon_ctx *ctx;
    cylon_status rc = cylon_open(&ctx, &cfg);
    if (rc != CYLON_ST_OK) {
        fprintf(stderr, "cylon_open: %s\n", cylon_strerror(rc));
        return 1;
    }
    rc = cylon_load(ctx);
    if (rc != CYLON_ST_OK) {
        fprintf(stderr, "cylon_load: %s\n", cylon_strerror(rc));
        return 1;
    }
    uint32_t dim = cylon_dim(ctx);
    printf("autof_check: f=%s dim=%u ntotal=%lu n=%u batch=%u\n",
           auto_f ? "auto" : "fixed", dim,
           (unsigned long)cylon_ntotal(ctx), nq, batch);
    uint16_t *q = malloc((size_t)nq * dim * 2);
    uint32_t *ids = malloc((size_t)nq * k * 4);
    float *dist = malloc((size_t)nq * k * 4);
    FILE *fp = fopen(qfile, "rb");
    if (!q || !ids || !dist || !fp ||
        fread(q, 2, (size_t)nq * dim, fp) != (size_t)nq * dim) {
        fprintf(stderr, "alloc/read fail\n");
        return 1;
    }
    fclose(fp);
    FILE *dfp = fopen(dumpfile, "wb");
    if (!dfp) {
        fprintf(stderr, "open %s\n", dumpfile);
        return 1;
    }
    for (uint32_t off = 0; off < nq; off += batch) {
        uint32_t nb = (off + batch <= nq) ? batch : nq - off;
        double t0 = now_s();
        cylon_stats st;
        rc = cylon_search(ctx, CYLON_PREC_F16, q + (size_t)off * dim, nb,
                          k, ef, ids + (size_t)off * k,
                          dist + (size_t)off * k, &st);
        double wall = now_s() - t0;
        if (rc != CYLON_ST_OK) {
            fprintf(stderr, "cylon_search batch@%u: %s\n", off,
                    cylon_strerror(rc));
            return 1;
        }
        last_f = st.f_cur;
        printf("batch@%u nb=%u wall=%.3fs f=%.3f dist=%lu hops=%lu "
               "engine_ns=%lu\n", off, nb, wall, st.f_cur,
               (unsigned long)st.n_dist, (unsigned long)st.n_hops,
               (unsigned long)st.engine_ns);
        for (uint32_t qi = 0; qi < nb; qi++) {
            uint32_t m = 0;
            for (uint32_t jj = 0; jj < k; jj++) {
                if (ids[(size_t)(off + qi) * k + jj] != 0xffffffffu) m++;
            }
            fwrite(&m, 4, 1, dfp);
            fwrite(ids + (size_t)(off + qi) * k, 4, k, dfp);
        }
    }
    fclose(dfp);
    printf("dump written: %s (%lu B)\n", dumpfile,
           (unsigned long)(nq * (4 + k * 4)));
    cylon_close(ctx);
    if (auto_f) {
        printf("AUTO-F-FINAL f=%.3f %s\n", last_f,
               (fabs(last_f - 0.5) <= 0.1) ? "CONVERGED" : "DIVERGED");
    } else {
        printf("FIXED-F-DONE f=%.3f\n", fixed_f);
    }
    return 0;
}
