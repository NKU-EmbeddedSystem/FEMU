/*
 * c_min.c -- minimal libcylon consumer: open -> load -> search -> close.
 * M1 gate client: F16 pass-through, fixed f=0.5, writes the experiment
 * dump format (u32 m + k x u32 ids per query) for cmp vs engref.
 */
#include "cylon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int main(int argc, char **argv)
{
    const char *blob, *qfile, *dump;
    uint32_t nq, k, ef, dim, *ids, m;
    float *dist;
    cylon_ctx *ctx;
    cylon_config cfg;
    cylon_info info;
    cylon_stats stats;
    uint16_t *q;
    const char *window = NULL;
    FILE *f;
    if (argc < 5) {
        fprintf(stderr, "usage: %s -f blob -q queries_fp16 -o dump "
                        "[-n N -k K -e EF] [-w /dev/dax0.0]\n", argv[0]);
        return 1;
    }
    blob = qfile = dump = NULL;
    nq = 1000; k = 10; ef = 100;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) { blob = argv[++i]; }
        else if (!strcmp(argv[i], "-q")) { qfile = argv[++i]; }
        else if (!strcmp(argv[i], "-o")) { dump = argv[++i]; }
        else if (!strcmp(argv[i], "-n")) { nq = strtoul(argv[++i], 0, 0); }
        else if (!strcmp(argv[i], "-k")) { k = strtoul(argv[++i], 0, 0); }
        else if (!strcmp(argv[i], "-e")) { ef = strtoul(argv[++i], 0, 0); }
        else if (!strcmp(argv[i], "-w")) { window = argv[++i]; }
        else { fprintf(stderr, "bad arg %s\n", argv[i]); return 1; }
    }
    if (!blob || !qfile || !dump) {
        fprintf(stderr, "usage: %s -f blob -q queries_fp16 -o dump\n",
                argv[0]);
        return 1;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.blob_path = blob;
    cfg.window_dev = window;      /* NULL -> /dev/dax0.0 */
    cfg.cpu_frac = 0.5;           /* fixed split for the M1 byte gate */
    cfg.ef = ef;
    cfg.notify = CYLON_NOTIFY_POLL;
    cylon_status rc = cylon_open(&ctx, &cfg);
    if (rc != CYLON_ST_OK) {
        fprintf(stderr, "cylon_open: %d\n", rc);
        return 1;
    }
    rc = cylon_load(ctx);
    if (rc != CYLON_ST_OK) {
        fprintf(stderr, "cylon_load: %d\n", rc);
        return 1;
    }
    cylon_get_info(ctx, &info);
    dim = cylon_dim(ctx);
    printf("device: dim %u ntotal %lu storage f%d accum f%d metric %s "
           "in_prec_mask 0x%x\n", dim, (unsigned long)info.ntotal,
           info.storage_prec == CYLON_PREC_F16 ? 16 : 32,
           info.accum_prec == CYLON_PREC_F32 ? 32 : 16,
           info.metric, info.in_prec_mask);
    f = fopen(qfile, "rb");
    if (!f) {
        fprintf(stderr, "open %s\n", qfile);
        return 1;
    }
    q = malloc((size_t)nq * dim * 2);
    ids = malloc((size_t)nq * k * 4);
    dist = malloc((size_t)nq * k * 4);
    if (!q || !ids || !dist || fread(q, 2, (size_t)nq * dim, f) != (size_t)nq * dim) {
        fprintf(stderr, "short read on %s (want nq=%u dim=%u fp16)\n",
                qfile, nq, dim);
        return 1;
    }
    fclose(f);
    rc = cylon_search(ctx, CYLON_PREC_F16, q, nq, k, ef, ids, dist, &stats);
    if (rc != CYLON_ST_OK) {
        fprintf(stderr, "cylon_search: %d\n", rc);
        return 1;
    }
    printf("engine: dist %lu hops %lu pages %lu ns %lu\n",
           (unsigned long)stats.n_dist, (unsigned long)stats.n_hops,
           (unsigned long)stats.n_pages, (unsigned long)stats.engine_ns);
    /* dump in the experiment byte-gate format: u32 m + k x u32 ids */
    FILE *dfp = fopen(dump, "wb");
    if (!dfp) {
        fprintf(stderr, "open %s\n", dump);
        return 1;
    }
    for (uint32_t qi = 0; qi < nq; qi++) {
        m = 0;
        for (uint32_t jj = 0; jj < k; jj++) {
            if (ids[qi * k + jj] != 0xffffffffu) {
                m++;
            }
        }
        fwrite(&m, 4, 1, dfp);
        fwrite(ids + (size_t)qi * k, 4, k, dfp);
    }
    fclose(dfp);
    cylon_close(ctx);
    printf("dump written: %s (%lu B)\n", dump,
           (unsigned long)(nq * (4 + k * 4)));
    return 0;
}
