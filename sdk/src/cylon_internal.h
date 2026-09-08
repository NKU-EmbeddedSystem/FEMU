/*
 * cylon_internal.h -- shared declarations for libcylon TUs + the experiment
 * CLI. GENERATED (mechanically) by tools/split_cpu_search.py from the frozen
 * cpu_search.c monolith; struct bodies are verbatim extractions. Do not edit
 * the moved blocks by hand -- rerun the splitter instead.
 */
#ifndef CYLON_INTERNAL_H
#define CYLON_INTERNAL_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>

#include "pnm_uapi.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pnm_cnd {
    float d;
    uint32_t id;
};

struct collab_ctx {
    uint64_t qoff;
    uint64_t *lat;
    struct pnm_cnd *out;             /* scratch; only the worker touches it */
    uint32_t n_cpu, k, ef;
};

/* ---- state (graph points INTO the CXL window; algorithm state local) ---- */
struct cylon_st {
    uint32_t dim, count, maxm0;
    uint32_t entry_point;
    uint8_t entry_level;
    uint64_t g_off_vectors, g_off_adj0, g_off_upper, g_off_levels;
    uint8_t *graph;
    uint8_t *visited;
    struct pnm_cnd *cand;
    struct pnm_cnd *res;
    uint32_t cand_cap, res_cap;
    float *qconv;
};


extern struct cylon_st st;

/* set -DCYLON_BUILD_AVX=1 when building libcylon_avx.so; guards the
 * known avx x f=0.25 client crash family (CYLON_ST_EAVX refusal) */
#ifndef CYLON_BUILD_AVX
#define CYLON_BUILD_AVX 0
#endif

/* collab merged-result capacity (k <= this) */
#include "cylon.h"

/* k upper bound is ABI-level: alias the public constant */
#define COLLAB_KMAX CYLON_KMAX

/* platform state (cylon.c) */
extern uint8_t *win;
extern uint64_t win_blob_bytes, win_sz;
extern struct pnm_mb_s *mb;
extern uint32_t g_poll_us;
extern int g_db_fd;
extern uint32_t gen_ctr, jid;

/* traversal + collab state (cylon.c) */
extern int g_trace;
extern uint64_t g_dist, g_hops;
extern uint64_t g_e_dist, g_e_hops, g_e_pages, g_e_ns;
extern uint32_t (*g_all_ids)[COLLAB_KMAX];
extern float (*g_all_d)[COLLAB_KMAX];
extern uint32_t *g_all_m;

uint64_t now_ns(void);
void mb_reset(void);
uint32_t mb_submit(uint32_t op, uint64_t a0, uint64_t a1,
                          uint32_t k, uint32_t ef, uint64_t timeout_ns,
                          struct pnm_resp_s *r);
uint64_t stage_file(const char *path, uint64_t off);
uint32_t stage_device(uint64_t blob_bytes, uint32_t *staged_pages);
int ping(void);
int flush_cache(void);
int bind_index(void);
int verify_window(const char *blob);
int engine_feeder(uint64_t qoff, uint32_t n, uint32_t n_cpu,
                  uint32_t k, uint32_t ef, uint64_t *lat);
void *cpu_worker(void *arg);
int cmp_u64(const void *a, const void *b);

#ifdef __cplusplus
}
#endif
#endif /* CYLON_INTERNAL_H */
