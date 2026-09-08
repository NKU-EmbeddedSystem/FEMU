/*
 * cylon_api.c -- public cylon.h implementation on top of the moved
 * cpu_search.c core (cylon.c). Single-ctx: flock on /tmp/cylon-window.lock
 * guards the device for the ctx lifetime. Device profile (CYH1 v1):
 * fp16 storage / fp32 accumulate / L2 -- reported via cylon_get_info,
 * never silently assumed: search() validates prec against in_prec_mask.
 */
#include "cylon_internal.h"
#include "cylon.h"
#include <math.h>

/* this engine build's v1 profile: fp16 storage / fp32 accum / L2; accepted
 * inputs = F32 (library converts to F16, one RNE rounding per query vector)
 * and F16 (pass-through). Disclosed via cylon_get_info, never assumed. */
#define V1_IN_PREC_MASK \
    (CYLON_PREC_BIT(CYLON_PREC_F32) | CYLON_PREC_BIT(CYLON_PREC_F16))

/* f32 -> f16, IEEE 754 round-to-nearest-even. Semantic reference is numpy
 * astype(float16) (that is how queries_fp16.bin was produced); verified
 * against numpy on the host (sdk/tests/test_f2h.py). */
static uint16_t f32_to_f16(float f)
{
    union { uint32_t u; float f; } v;
    v.f = f;
    uint32_t x = v.u;
    uint32_t sign = (x >> 16) & 0x8000u;
    x &= 0x7fffffffu;
    if (x >= 0x7f800000u) {          /* Inf / NaN: payload = m>>13, and a
                                      * nonzero payload that truncates to 0
                                      * keeps mantissa 1 (numpy semantics;
                                      * low payload bits are lost) */
        uint32_t m = x & 0x7fffffu;
        uint32_t hm = m >> 13;
        if (!hm && m) {
            hm = 1;
        }
        return (uint16_t)(sign | 0x7c00u | hm);
    }
    if (x >= 0x477ff000u)            /* overflow: RNE -> Inf */
        return (uint16_t)(sign | 0x7c00u);
    if (x >= 0x38800000u) {          /* normal: RNE at bit 13 */
        x += 0x0fffu + ((x >> 13) & 1u);
        return (uint16_t)(sign | ((x - 0x38000000u) >> 13));
    }
    if (x < 0x33000000u)             /* underflow to zero */
        return (uint16_t)sign;
    {                                 /* subnormal: RNE */
        int32_t shift = 126 - (int32_t)(x >> 23);
        uint32_t m = (x & 0x7fffffu) | 0x00800000u;
        uint32_t r = m >> shift, rem = m & ((1u << shift) - 1u), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (r & 1u)))
            r++;
        return (uint16_t)(sign | r);
    }
}

/* ---------- single-ctx device lock ---------- */
static int g_lock_fd = -1;

static int take_lock(void)
{
    if (g_lock_fd >= 0) {
        return 0;                        /* already held by this process */
    }
    g_lock_fd = open("/tmp/cylon-window.lock", O_CREAT | O_RDWR, 0666);
    if (g_lock_fd < 0) {
        return -1;
    }
    if (flock(g_lock_fd, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "cylon: device busy (another ctx holds the window)\n");
        close(g_lock_fd);
        g_lock_fd = -1;
        return -1;
    }
    return 0;
}

static void drop_lock(void)
{
    if (g_lock_fd >= 0) {
        close(g_lock_fd);
        g_lock_fd = -1;
    }
}

/* ---------- window helpers (main() keeps its own verbatim inline copy) ---- */
static const char *g_dev;
static cylon_config g_cfg;

static int window_open(const char *dev)
{
    int fd;
    off_t ws;
    g_dev = dev;
    fd = open(dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "cylon: open %s: %s\n", dev, strerror(errno));
        return -1;
    }
    ws = lseek(fd, 0, SEEK_END);
    if (ws <= 0) {
        char p[256];
        snprintf(p, sizeof(p), "/sys/bus/dax/devices/%s/size", strrchr(dev, '/') + 1);
        FILE *sf = fopen(p, "r");
        if (sf) {
            unsigned long long v = 0;
            if (fscanf(sf, "%llu", &v) == 1) {
                ws = (off_t)v;
            }
            fclose(sf);
        }
    }
    if (ws <= 0) {
        fprintf(stderr, "cylon: cannot determine %s size\n", dev);
        close(fd);
        return -1;
    }
    win_sz = (uint64_t)ws;
    win = mmap(NULL, win_sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (win == MAP_FAILED) {
        fprintf(stderr, "cylon: mmap %s (%lu B): %s\n", dev,
                (unsigned long)win_sz, strerror(errno));
        close(fd);
        return -1;
    }
    mb = (struct pnm_mb_s *)(win + win_sz - PNM_MB_OFF_FROM_END);
    return 0;
}

static int doorbell_open(cylon_notify_mode mode)
{
    int db_requested = (mode == CYLON_NOTIFY_DOORBELL) ||
                  (mode == CYLON_NOTIFY_AUTO);
    (void)db_requested;
    if (mode == CYLON_NOTIFY_POLL) {
        return 0;                        /* tight spin */
    }
    g_db_fd = open("/dev/cylon-db", O_RDWR);
    if (g_db_fd < 0) {
        if (mode == CYLON_NOTIFY_DOORBELL) {
            fprintf(stderr, "cylon: doorbell mode: open /dev/cylon-db: %s\n",
                    strerror(errno));
            return -1;                   /* hard error, as the CLI does */
        }
        printf("cylon: doorbell unavailable (%s) -> poll fallback\n",
               strerror(errno));
        return 0;                        /* AUTO soft fallback to spin */
    }
    return 0;
}

/* ---------- context ---------- */
struct cylon_ctx {
    cylon_config cfg;        /* normalized copy */
    int loaded;
    int dead;                /* ETO poison: engine wedged, close+open */
    double f_cur;            /* effective collab split (auto or fixed) */
    int has_auto;            /* cpu_frac=NAN -> max-model batch update */
    uint64_t *lat;           /* per-query latency scratch (nq slots) */
    uint32_t lat_alloc;
};


cylon_status cylon_open(cylon_ctx **out, const cylon_config *cfg_in)
{
    cylon_config c;
    cylon_ctx *ctx;

    if (!out) {
        return CYLON_ST_EINVAL;
    }
    *out = NULL;
    memset(&c, 0, sizeof(c));
    if (cfg_in) {
        c = *cfg_in;
    } else {
        c.cpu_frac = NAN;                /* auto-f */
    }
    if (!c.window_dev) {
        c.window_dev = "/dev/dax0.0";
    }
    if (!c.ef) {
        c.ef = 100;
    }
    if (c.cpu_frac < 0.0 || c.cpu_frac > 1.0) {
        return CYLON_ST_EINVAL;
    }
    if (CYLON_BUILD_AVX && fabs(c.cpu_frac - 0.25) < 1e-9) {
        return CYLON_ST_EAVX;            /* known client crash family */
    }
    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return CYLON_ST_EINVAL;
    }
    if (take_lock() < 0) {
        free(ctx);
        return CYLON_ST_EBUSY;
    }
    if (window_open(c.window_dev) < 0) {
        drop_lock();
        free(ctx);
        return CYLON_ST_ENODEV;
    }
    if (doorbell_open(c.notify) < 0) {
        munmap(win, win_sz);
        win = NULL;
        drop_lock();
        free(ctx);
        return CYLON_ST_ENODEV;
    }
    if (ping() != 0) {
        /* engine not answering the NOP resync frame: wedged -> re-open */
        if (g_db_fd >= 0) {
            close(g_db_fd);
            g_db_fd = -1;
        }
        munmap(win, win_sz);
        win = NULL;
        drop_lock();
        free(ctx);
        return CYLON_ST_ETO;
    }
    ctx->cfg = c;
    ctx->has_auto = isnan(c.cpu_frac);
    ctx->f_cur = ctx->has_auto ? 0.5 : c.cpu_frac;
    *out = ctx;
    return CYLON_ST_OK;
}

/* ---------- load: FLUSH + stage (dev-first, ft fallback) + BIND ---------- */
cylon_status cylon_load(cylon_ctx *ctx)
{
    struct cyh1_header *hdr;
    struct stat sb;
    uint64_t blob_bytes, want, staged_pages;
    uint32_t rc;

    if (!ctx || ctx->dead) {
        return ctx && ctx->dead ? CYLON_ST_ETO : CYLON_ST_EINVAL;
    }
    if (!ctx->cfg.blob_path) {
        fprintf(stderr, "cylon: load(): cfg.blob_path not set\n");
        return CYLON_ST_EINVAL;
    }
    if (stat(ctx->cfg.blob_path, &sb) < 0) {
        fprintf(stderr, "cylon: stat %s: %s\n", ctx->cfg.blob_path,
                strerror(errno));
        return CYLON_ST_EINVAL;
    }
    blob_bytes = (uint64_t)sb.st_size;
    if (blob_bytes + 4096 > win_sz) {
        fprintf(stderr, "cylon: blob (%lu MB) does not fit window (%lu MB)\n",
                (unsigned long)(blob_bytes >> 20),
                (unsigned long)(win_sz >> 20));
        return CYLON_ST_EINVAL;
    }
    /* load sequence: ping -> FLUSH (stale-EPTE guarantee, #27) -> stage.
     * stage_file's exit(2) paths (missing file / no fit) are pre-checked
     * above, so the library cannot abort the host process. */
    if (ping() || flush_cache()) {
        ctx->dead = 1;
        return CYLON_ST_ETO;
    }
    /* D2 device-initiated staging first; first-touch push fallback (the
     * legacy path the CLI uses) on shortfall / ENOSYS / partial cover.
     * Timeout in stage_device() exits (1h; unreachable on healthy hw). */
    want = (blob_bytes + 4095) >> 12;
    staged_pages = 0;
    rc = stage_device(blob_bytes, (uint32_t *)&staged_pages);
    if (rc == PNM_ST_OK && staged_pages == want) {
        printf("cylon: device-staged blob: %u pages (%lu MB) via PNM_OP_STAGE\n",
               (unsigned)staged_pages, (unsigned long)(blob_bytes >> 20));
    } else if (rc == PNM_ST_EINVAL) {
        fprintf(stderr, "cylon: device staging EINVAL (blob too big?)\n");
        return CYLON_ST_EINVAL;
    } else {
        printf("cylon: device staging covered %u/%lu pages (rc=%u) -- "
               "first-touch fallback\n",
               (unsigned)staged_pages, (unsigned long)want, rc);
        flush_cache();
        blob_bytes = stage_file(ctx->cfg.blob_path, 0);
    }
    win_blob_bytes = blob_bytes;

    /* CYH1 header + traversal state (verbatim setup from the CLI flow) */
    hdr = (struct cyh1_header *)win;
    if (hdr->magic != CYH1_MAGIC) {
        fprintf(stderr, "cylon: bad CYH1 magic %08x\n", hdr->magic);
        return CYLON_ST_EINVAL;
    }
    st.graph = win;
    st.dim = hdr->dim;  st.count = hdr->count;  st.maxm0 = hdr->maxm0;
    st.entry_point = hdr->entry_point;
    st.entry_level = (uint8_t)hdr->entry_level;
    st.g_off_vectors = hdr->off_vectors;
    st.g_off_adj0 = hdr->off_adj0;
    st.g_off_upper = hdr->off_upper;
    st.g_off_levels = hdr->off_levels;

    /* re-load() after a prior load: drop the previous traversal state */
    free(st.visited);   st.visited = NULL;
    free(st.cand);      st.cand = NULL;
    free(st.res);       st.res = NULL;
    free(st.qconv);     st.qconv = NULL;

    st.visited = calloc(1, (st.count + 7) / 8);
    st.cand_cap = 65536; st.res_cap = 4096;
    st.cand = malloc(sizeof(struct pnm_cnd) * st.cand_cap);
    st.res = malloc(sizeof(struct pnm_cnd) * st.res_cap);
    st.qconv = malloc(sizeof(float) * 4096);
    if (!st.visited || !st.cand || !st.res || !st.qconv) {
        fprintf(stderr, "cylon: OOM traversal state\n");
        return CYLON_ST_EINVAL;
    }

    /* engine needs the index bound once (BIND walks the whole staged blob) */
    if (bind_index()) {
        ctx->dead = 1;
        return CYLON_ST_ETO;
    }
    ctx->loaded = 1;
    printf("cylon: loaded %s: dim %u count %u maxm0 %u entry %u lvl %u\n",
           ctx->cfg.blob_path, st.dim, st.count, st.maxm0, st.entry_point,
           st.entry_level);
    return CYLON_ST_OK;
}

/* stage a caller buffer into the window at 'off' with the same scalar
 * per-page first-touch discipline as stage_file() (VEX first-touch of a
 * trapped page is unemulatable, #20/#36). Returns 0 on success, -1 on fit
 * failure (caller pre-checks normally make this unreachable). */
static int stage_buf(const uint16_t *src, uint64_t n, uint64_t off)
{
    uint64_t done = 0;
    while (done < n) {
        uint8_t *d = win + off + done;
        uint64_t in_pg = 4096 - ((uintptr_t)d & 0xfff);
        uint64_t chunk = n - done < in_pg ? n - done : in_pg;
        if (((uintptr_t)d & 0xfff) == 0) {
            if (chunk >= 8) {
                *(volatile uint64_t *)d = *(const uint64_t *)(src + done / 2);
            } else {
                *(volatile uint8_t *)d = *(const uint8_t *)(src + done / 2);
            }
        }
        memcpy(d, (const uint8_t *)src + done, (size_t)chunk);
        done += chunk;
    }
    return 0;
}

/* ---------- search: prec convert -> stage -> collab drive -> copy-out --- */
cylon_status cylon_search(cylon_ctx *ctx, cylon_prec prec,
                          const void *queries, uint32_t nq,
                          uint32_t k, uint32_t ef,
                          uint32_t *out_ids, float *out_dist,
                          cylon_stats *stats)
{
    struct collab_ctx cc;
    pthread_t thr;
    uint16_t *conv = NULL;
    const uint16_t *q16;
    uint64_t qoff, qbytes, avail, res_off, *lat;
    uint64_t cpu_sum = 0, eng_sum = 0;
    uint32_t n_cpu, n_eng;
    uint32_t i, j;
    double f, t_c, t_e;

    if (!ctx || ctx->dead) {
        return ctx && ctx->dead ? CYLON_ST_ETO : CYLON_ST_EINVAL;
    }
    if (!ctx->loaded || !queries || !out_ids || !out_dist) {
        return CYLON_ST_EINVAL;
    }
    if (!k || k > COLLAB_KMAX) {
        return CYLON_ST_EINVAL;
    }
    if (!nq) {
        if (stats) {
            memset(stats, 0, sizeof(*stats));
        }
        return CYLON_ST_OK;
    }
    /* precision contract: validate against the device's accepted input set,
     * never silently downgrade (see cylon_info.in_prec_mask) */
    if (!(CYLON_PREC_BIT(prec) & V1_IN_PREC_MASK)) {
        return CYLON_ST_EPREC;
    }
    ef = ef ? ef : ctx->cfg.ef;
    f = ctx->has_auto ? ctx->f_cur : ctx->cfg.cpu_frac;
    if (CYLON_BUILD_AVX && fabs(f - 0.25) < 1e-9) {
        return CYLON_ST_EAVX;
    }
    /* window layout (frozen): queries at blob tail, page-aligned; results
     * at win_sz-PNM_RESULTS_OFF_FROM_END; mailbox last page */
    res_off = win_sz - PNM_RESULTS_OFF_FROM_END;
    qoff = (win_blob_bytes + 4095) & ~4095ull;
    qbytes = (uint64_t)nq * st.dim * 2;
    avail = res_off - qoff;
    if (qbytes > avail) {
        fprintf(stderr, "cylon: queries (%lu B) exceed window budget %lu B "
                        "(nq=%u dim=%u)\n", (unsigned long)qbytes,
                (unsigned long)avail, nq, st.dim);
        return CYLON_ST_EINVAL;
    }
    /* F32 input: one RNE rounding per query vector into a scratch buffer;
     * F16 input: pass-through (engine and traversal read fp16 verbatim) */
    if (prec == CYLON_PREC_F32) {
        const float *q32 = queries;
        conv = malloc((size_t)nq * st.dim * 2);
        if (!conv) {
            return CYLON_ST_EINVAL;
        }
        for (i = 0; i < nq * st.dim; i++) {
            conv[i] = f32_to_f16(q32[i]);
        }
        q16 = conv;
    } else {
        q16 = queries;
    }
    /* stage the query block (scalar per-page first-touch discipline — VEX
     * first-touch of a trapped page is unemulatable, #20/#36) */
    if (stage_buf(q16, qbytes, qoff) < 0) {
        free(conv);
        return CYLON_ST_EINVAL;
    }
    /* collab drive: single CPU worker walking [0, n_cpu) serially (M1
     * fidelity = current client), feeder = calling thread, [n_cpu, nq) */
    n_cpu = (uint32_t)(f * (double)nq + 0.5);
    if (n_cpu > nq) {
        n_cpu = nq;
    }
    n_eng = nq - n_cpu;
    g_all_ids = calloc(nq, sizeof(*g_all_ids));
    g_all_d = calloc(nq, sizeof(*g_all_d));
    g_all_m = calloc(nq, 4);
    if (!g_all_ids || !g_all_d || !g_all_m) {
        free(conv);
        free(g_all_ids); free(g_all_d); free(g_all_m);
        g_all_ids = NULL; g_all_d = NULL; g_all_m = NULL;
        return CYLON_ST_EINVAL;
    }
    lat = ctx->lat;
    if (ctx->lat_alloc < nq) {
        lat = realloc(ctx->lat, (size_t)nq * 8);
        if (!lat) {
            free(conv);
            free(g_all_ids); free(g_all_d); free(g_all_m);
            g_all_ids = NULL; g_all_d = NULL; g_all_m = NULL;
            return CYLON_ST_EINVAL;
        }
        ctx->lat = lat;
        ctx->lat_alloc = nq;
    }
    memset(&cc, 0, sizeof(cc));
    cc.qoff = qoff;
    cc.lat = lat;
    cc.out = malloc(sizeof(struct pnm_cnd) * k);
    if (!cc.out) {
        free(conv);
        free(g_all_ids); free(g_all_d); free(g_all_m);
        g_all_ids = NULL; g_all_d = NULL; g_all_m = NULL;
        return CYLON_ST_EINVAL;
    }
    cc.k = k;
    cc.ef = ef;
    cc.n_cpu = n_cpu;

    /* engine-side failure poisons the ctx (close+open to recover) */
    bool have_thr = n_cpu > 0;
    if (have_thr && pthread_create(&thr, NULL, cpu_worker, &cc) != 0) {
        fprintf(stderr, "cylon: pthread_create: %s\n", strerror(errno));
        free(cc.out);
        free(conv);
        free(g_all_ids); free(g_all_d); free(g_all_m);
        g_all_ids = NULL; g_all_d = NULL; g_all_m = NULL;
        return CYLON_ST_EINVAL;
    }

    if (engine_feeder(qoff, nq, n_cpu, k, ef, lat)) {
        if (have_thr) {
            pthread_join(thr, NULL);
        }
        free(cc.out);
        free(conv);
        free(g_all_ids); free(g_all_d); free(g_all_m);
        g_all_ids = NULL; g_all_d = NULL; g_all_m = NULL;
        ctx->dead = 1;
        return CYLON_ST_ETO;
    }
    if (have_thr) {
        pthread_join(thr, NULL);
    }

    /* copy-out: rows already normalized engine-side/cpu-side (slots beyond
     * n_found = id 0xffffffff / dist 0.0f), so (nq,k) fills exactly and m
     * stays reconstructible for byte-gates */
    for (i = 0; i < nq; i++) {
        memcpy(out_ids + (size_t)i * k, g_all_ids[i], (size_t)k * 4);
        memcpy(out_dist + (size_t)i * k, g_all_d[i], (size_t)k * 4);
    }
    if (stats) {
        stats->n_dist = g_e_dist;
        stats->n_hops = g_e_hops;
        stats->n_pages = g_e_pages;
        stats->engine_ns = g_e_ns;
    }

    /* auto-f: batch-level max-model update, f_new = t_e / (t_c + t_e) with
     * per-query averages (balance = equal per-query times). Skip the
     * degenerate arms (all-CPU / all-engine) */
    if (ctx->has_auto && n_cpu > 0 && n_eng > 0) {
        cpu_sum = 0; eng_sum = 0;
        for (i = 0; i < n_cpu; i++) {
            cpu_sum += lat[i];
        }
        for (i = n_cpu; i < nq; i++) {
            eng_sum += lat[i];
        }
        t_c = (double)cpu_sum / n_cpu;
        t_e = (double)eng_sum / n_eng;
        f = t_e / (t_c + t_e);
        if (f < 0.05) f = 0.05;
        if (f > 0.95) f = 0.95;
        if (!(CYLON_BUILD_AVX && fabs(f - 0.25) < 1e-9)) {
            ctx->f_cur = f;
        }
        printf("cylon: auto-f -> %.3f (t_c %.3fs t_e %.3fs per-query)\n",
               ctx->f_cur, t_c, t_e);
    }
    if (stats) {
        stats->f_cur = (float)ctx->f_cur;
    }
    free(conv);
    free(g_all_ids); free(g_all_d); free(g_all_m);
    g_all_ids = NULL; g_all_d = NULL; g_all_m = NULL;
    return CYLON_ST_OK;
}

cylon_status cylon_close(cylon_ctx *ctx)
{
    if (!ctx) {
        return CYLON_ST_EINVAL;
    }
    free(st.visited);   st.visited = NULL;
    free(st.cand);      st.cand = NULL;
    free(st.res);       st.res = NULL;
    free(st.qconv);     st.qconv = NULL;
    free(ctx->lat);     ctx->lat = NULL;
    free(g_all_ids);    g_all_ids = NULL;
    free(g_all_d);      g_all_d = NULL;
    free(g_all_m);      g_all_m = NULL;
    if (win) {
        munmap(win, win_sz);
        win = NULL;
    }
    if (g_db_fd >= 0) {
        close(g_db_fd);
        g_db_fd = -1;
    }
    drop_lock();
    free(ctx);
    return CYLON_ST_OK;
}

/* ---------- disclosure ---------- */
cylon_status cylon_get_info(const cylon_ctx *ctx, cylon_info *out)
{
    if (!ctx || !out) {
        return CYLON_ST_EINVAL;
    }
    out->storage_prec = CYLON_PREC_F16;   /* v1 profile, engine-side fact */
    out->accum_prec = CYLON_PREC_F32;
    out->metric = "l2";
    out->in_prec_mask = V1_IN_PREC_MASK;
    out->dim = ctx->loaded ? st.dim : 0;
    out->ntotal = ctx->loaded ? st.count : 0;
    return CYLON_ST_OK;
}

uint32_t cylon_dim(const cylon_ctx *ctx)
{
    return ctx && ctx->loaded ? st.dim : 0;
}

uint64_t cylon_ntotal(const cylon_ctx *ctx)
{
    return ctx && ctx->loaded ? st.count : 0;
}

const char *cylon_strerror(cylon_status st)
{
    switch (st) {
    case CYLON_ST_OK:     return "ok";
    case CYLON_ST_EINVAL: return "invalid argument/config";
    case CYLON_ST_ENODEV: return "device/window unreachable";
    case CYLON_ST_ETO:    return "engine timeout (close+open to recover)";
    case CYLON_ST_EAVX:   return "avx build at f=0.25 poison point";
    case CYLON_ST_EPREC:  return "requested input precision not in "
                                 "info.in_prec_mask";
    case CYLON_ST_EBUSY:  return "device busy (another ctx holds the window)";
    }
    return "unknown status";
}
