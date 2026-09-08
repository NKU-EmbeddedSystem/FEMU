/*
 * cylon.h — public C ABI for libcylon: the user-space client library for the
 * Cylon CXL-SSD ANNS accelerator (Type-3 story; mailbox ABI = pnm_uapi.h).
 *
 * Layering (see CYLON-SDK.md): app -> adapters -> this C ABI -> platform layer
 * (window mmap / staging / mailbox v2 / doorbell). Engine (pnm.c) and the
 * mailbox ABI are frozen; all SDK code is client-side.
 *
 * Precision contract: input precision is a per-search parameter; the device
 * storage/accumulate profile is a device fact reported via cylon_info. The
 * SDK never silently downgrades precision (unsupported = CYLON_ST_EPREC).
 */
#ifndef CYLON_H
#define CYLON_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cylon_ctx cylon_ctx;

typedef enum {
    CYLON_ST_OK = 0,
    CYLON_ST_EINVAL,      /* bad arguments / config */
    CYLON_ST_ENODEV,      /* window/doorbell device unreachable */
    CYLON_ST_ETO,         /* engine timeout / state machine wedged (re-open) */
    CYLON_ST_EAVX,        /* avx build x f=0.25 poison point, refused */
    CYLON_ST_EPREC,       /* requested input precision not in device profile */
    CYLON_ST_EBUSY        /* device already owned by another ctx (flock) */
} cylon_status;

/* Input precision namespace for the whole accelerator family; this simulator
 * implements F32 (library converts to device storage precision) and F16
 * (pass-through). Others are reserved placeholders for future profiles. */
typedef enum {
    CYLON_PREC_F32  = 0,  /* universal entry: library converts to storage prec */
    CYLON_PREC_F16,       /* half-precision pass-through */
    CYLON_PREC_BF16,      /* reserved: bfloat16 profile */
    CYLON_PREC_F64,       /* reserved: double profile */
    CYLON_PREC_I8         /* reserved: quantized profile (scale/zero-point TBD) */
} cylon_prec;

#define CYLON_PREC_BIT(p)  (1u << (p))

/* Wait mode for job completion. AUTO = doorbell if /dev/cylon-db is present,
 * else tight spin (v1 semantics). POLL = v1 tight spin (experiment control). */
typedef enum {
    CYLON_NOTIFY_AUTO = 0,
    CYLON_NOTIFY_POLL,
    CYLON_NOTIFY_DOORBELL
} cylon_notify_mode;

typedef struct {
    const char *window_dev;      /* NULL = "/dev/dax0.0" */
    const char *blob_path;       /* CYH1 blob path (guest-visible path) */
    double      cpu_frac;        /* collab split; NAN = auto (max model) */
    uint32_t    n_cpu_threads;   /* 0 = auto; M1 fidelity = 1 worker either way */
    uint32_t    ef;              /* default ef for search (ef=0 per call -> this) */
    cylon_notify_mode notify;
    uint32_t    stage_bps;       /* 0 = engine default (2GB/s model) */
} cylon_config;

/* Device self-description (open time, no load needed). The SDK does not
 * hardcode the device profile: it reports what the device is. */
typedef struct {
    uint32_t   dim;
    uint64_t   ntotal;
    cylon_prec storage_prec;   /* index vector storage precision (v1 = F16) */
    cylon_prec accum_prec;     /* distance accumulation precision (v1 = F32) */
    const char *metric;        /* "l2" */
    uint32_t   in_prec_mask;   /* acceptable input precisions (CYLON_PREC_BIT) */
} cylon_info;

/* Engine response counters, passed through unmodified (paper-experiment
 * surface; n_dist/n_hops/n_pages feed the misses->wall predictions).
 * f_cur = the collab split in effect for THIS batch (auto mode: the
 * post-update value; fixed mode: cfg cpu_frac). */
typedef struct {
    uint64_t n_dist, n_hops, n_pages, engine_ns;
    float    f_cur;
} cylon_stats;

/* ---- lifecycle ---- */
cylon_status cylon_open(cylon_ctx **out, const cylon_config *cfg);
cylon_status cylon_load(cylon_ctx *ctx);          /* FLUSH + stage + BIND */

/* Input precision as a first-class search parameter. queries = (nq, dim) in
 * 'prec'; outputs row-major (nq, k): ids (u32, CYH1 label space, 0xffffffff
 * = empty slot) + dists (f32 L2^2). ef=0 -> ctx default. stats may be NULL.
 * NOTE: avx-library builds refuse f=0.25 with CYLON_ST_EAVX (known client
 * crash family, E1''/E-M); the experiment CLI is NOT restricted this way. */
cylon_status cylon_search(cylon_ctx *ctx, cylon_prec prec,
                          const void *queries, uint32_t nq,
                          uint32_t k, uint32_t ef,
                          uint32_t *out_ids, float *out_dist,
                          cylon_stats *stats);

cylon_status cylon_close(cylon_ctx *ctx);

/* ---- disclosure ---- */
cylon_status cylon_get_info(const cylon_ctx *ctx, cylon_info *out);

/* convenience accessors (header-derived) */
uint32_t cylon_dim(const cylon_ctx *ctx);
uint64_t cylon_ntotal(const cylon_ctx *ctx);

/* device search contract: k upper bound (must match the engine; the
 * moved core's COLLAB_KMAX is the same number via cylon_internal.h) */
#define CYLON_KMAX 64

/* readable status text (C++/Python adapter layers; never NULL) */
const char *cylon_strerror(cylon_status st);

#ifdef __cplusplus
}
#endif
#endif /* CYLON_H */
