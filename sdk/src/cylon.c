/*
 * cpu_search.c — CPU-side ANNS worker over the CXL window.
 *
 * Purpose 1 (baseline): "CXL SSD 只暂存数据、CPU 全算" — the same CYH1
 * traversal the FEMU PNM engine runs (verbatim copy from pnm.c via
 * engref.c), executed on the guest vCPU, with ALL graph reads served
 * through the CXL window: EPT-direct on cached pages, trap->FTL->NAND
 * (~40us/page) on miss. Uses the identical staging/FLUSH mailbox-control
 * protocol as pnm_client, so latency compares apples-to-apples against
 * engine-side runs (ca*).
 *
 * Purpose 2 (collaborative mode, -F): queries [0, n_cpu) run on the guest
 * vCPU (worker pthread running the local pnm_search) while [n_cpu, n) are
 * offloaded through the mailbox to the FEMU engine (feeder on the main
 * thread, one outstanding job, results read back from the window results
 * area). Both sides share the device cache/FTL concurrently; per-query
 * results merge into one recall@k. -F 1.0 (default) = pure CPU baseline
 * (no thread, no BIND, byte-identical results to the old serial loop).
 *
 * Build — CRITICAL: two variants.
 *   Scalar-safe (use whenever the cache is smaller than the index, i.e.
 *   pages get evicted back to trap and re-touched mid-search; a vectorized
 *   load first-touching a trapped page cannot be emulated -> guest crash):
 *     gcc -O2 -fno-tree-vectorize -pthread -o cpu_search cpu_search.c
 *   Full-speed (ONLY when every access hits a direct EPTE, e.g. bufsz=512
 *   full residency after staging; quantifies the CPU SIMD compute knob):
 *     gcc -O3 -mavx2 -pthread -o cpu_search_vec cpu_search.c
 *
 * Run (guest): cpu_search -f blob -q queries -g gt -n N -k K -e EF -o dump
 *              [-F FRAC]  (FRAC = fraction of queries on the CPU, 0.0-1.0)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <poll.h>

#include "pnm_uapi.h"
#include "cylon_internal.h"

struct cylon_st st;


/* ================= copied verbatim from pnm.c (via engref.c) ================= */
static inline float pnm_f16_to_f32(uint16_t h)
{
    union { uint32_t u; float f; } cvt;
    const uint32_t sign = ((uint32_t)h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1fu;
    uint32_t man = h & 0x03ffu;

    if (exp == 0) {
        if (man == 0) {
            cvt.u = sign;
        } else {
            int b = 10;
            while (!((man >> b) & 1u)) {
                b--;
            }
            cvt.u = sign | ((uint32_t)(b + 103) << 23) |
                    ((man - (1u << b)) << (23 - b));
        }
    } else if (exp == 31) {
        cvt.u = sign | (0xffu << 23) | (man << 13);
    } else {
        cvt.u = sign | ((exp - 15 + 127) << 23) | (man << 13);
    }
    return cvt.f;
}


static inline void heap_swap(struct pnm_cnd *a, uint32_t i, uint32_t j)
{
    struct pnm_cnd t = a[i];
    a[i] = a[j];
    a[j] = t;
}

static void heap_push_min(struct pnm_cnd *h, uint32_t *n, uint32_t cap,
                          float d, uint32_t id)
{
    if (*n >= cap) {
        return;
    }
    uint32_t i = (*n)++;
    h[i].d = d;
    h[i].id = id;
    while (i && h[(i - 1) / 2].d > h[i].d) {
        heap_swap(h, (i - 1) / 2, i);
        i = (i - 1) / 2;
    }
}

static struct pnm_cnd heap_pop_min(struct pnm_cnd *h, uint32_t *n)
{
    struct pnm_cnd top = h[0];
    (*n)--;
    h[0] = h[*n];
    uint32_t i = 0;
    while (1) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < *n && h[l].d < h[m].d) {
            m = l;
        }
        if (r < *n && h[r].d < h[m].d) {
            m = r;
        }
        if (m == i) {
            break;
        }
        heap_swap(h, i, m);
        i = m;
    }
    return top;
}

static void heap_push_max(struct pnm_cnd *h, uint32_t *n, uint32_t cap,
                          float d, uint32_t id)
{
    if (*n >= cap) {
        return;
    }
    uint32_t i = (*n)++;
    h[i].d = d;
    h[i].id = id;
    while (i && h[(i - 1) / 2].d < h[i].d) {
        heap_swap(h, (i - 1) / 2, i);
        i = (i - 1) / 2;
    }
}

static struct pnm_cnd heap_pop_max(struct pnm_cnd *h, uint32_t *n)
{
    struct pnm_cnd top = h[0];
    (*n)--;
    h[0] = h[*n];
    uint32_t i = 0;
    while (1) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < *n && h[l].d > h[m].d) {
            m = l;
        }
        if (r < *n && h[r].d > h[m].d) {
            m = r;
        }
        if (m == i) {
            break;
        }
        heap_swap(h, i, m);
        i = m;
    }
    return top;
}



static inline float pnm_dist(uint32_t id)
{
    const uint16_t *v = (const uint16_t *)(st.graph + st.g_off_vectors) +
                        (size_t)id * st.dim;
    const float *q = st.qconv;
    float acc = 0.0f;
    for (uint32_t i = 0; i < st.dim; i++) {
        float diff = q[i] - pnm_f16_to_f32(v[i]);
        acc += diff * diff;
    }
    return acc;
}

static uint32_t pnm_neighbors(uint32_t id, int level, const uint32_t **ids)
{
    if (level == 0) {
        const uint32_t *a = (const uint32_t *)(st.graph + st.g_off_adj0) +
                            (size_t)id * st.maxm0;
        *ids = a;
        uint32_t degree = 0;
        while (degree < st.maxm0 && a[degree] != 0xffffffffu) {
            degree++;
        }
        return degree;
    }
    uint32_t off = ((const uint32_t *)(st.graph + st.g_off_upper))[id];
    if (!off) {
        *ids = NULL;
        return 0;
    }
    const uint8_t *r = (const uint8_t *)st.graph + st.g_off_upper + off;
    for (int l = 1; l < level; l++) {
        uint32_t d = *(const uint32_t *)r;
        (void)d;
        r += 4 + d * 4;
    }
    *ids = (const uint32_t *)(r + 4);
    return *(const uint32_t *)r;
}

static bool pnm_visit(uint32_t *cand_n, uint32_t *res_n,
                      uint32_t id, float d, uint32_t ef)
{
    if (st.visited[id >> 3] & (1u << (id & 7))) {
        return false;
    }
    st.visited[id >> 3] |= 1u << (id & 7);

    if (*res_n < ef) {
        heap_push_max(st.res, res_n, st.res_cap, d, id);
    } else if (d < st.res[0].d) {
        heap_pop_max(st.res, res_n);
        heap_push_max(st.res, res_n, st.res_cap, d, id);
    } else {
        return false;
    }
    heap_push_min(st.cand, cand_n, st.cand_cap, d, id);
    return true;
}
/* ================= end verbatim copy ================= */

static int pnm_cnd_asc(const void *a, const void *b)
{
    const struct pnm_cnd *ca = a, *cb = b;
    if (ca->d == cb->d) {
        /* canonical tie-break by id — must match pnm.c's pnm_cmp_asc */
        return ca->id < cb->id ? -1 : (ca->id > cb->id ? 1 : 0);
    }
    return ca->d < cb->d ? -1 : 1;
}

/* returns number found; fills out[k] (id,dist) pairs */
uint64_t g_dist, g_hops;
int g_trace;
static uint32_t pnm_search(const uint16_t *qh, uint32_t k, uint32_t ef,
                           struct pnm_cnd *out)
{
    for (uint32_t i = 0; i < st.dim; i++) {
        st.qconv[i] = pnm_f16_to_f32(qh[i]);
    }

    uint64_t n_dist = 1, n_hops = 0;
    uint32_t cur = st.entry_point;
    float cur_d = pnm_dist(cur);
    if (g_trace) printf("TRACE entry cur=%u d=%.1f lvl=%u\n", cur, cur_d, st.entry_level);
    for (int l = st.entry_level; l >= 1; l--) {
        bool improved = true;
        while (improved) {
            improved = false;
            const uint32_t *ids;
            uint32_t degree = pnm_neighbors(cur, l, &ids);
            n_hops++;
            if (g_trace) printf("TRACE lvl%d hop%llu cur=%u d=%.1f deg=%u\n",
                               l, (unsigned long long)n_hops, cur, cur_d, degree);
            for (uint32_t j = 0; j < degree; j++) {
                uint32_t nb = ids[j];
                float d = pnm_dist(nb);
                n_dist++;
                if (d < cur_d) {
                    cur_d = d;
                    cur = nb;
                    improved = true;
                }
            }
        }
    }

    uint32_t cand_n = 0, res_n = 0;
    memset(st.visited, 0, (st.count + 7) / 8);

    pnm_visit(&cand_n, &res_n, cur, cur_d, ef);

    while (cand_n) {
        struct pnm_cnd c = heap_pop_min(st.cand, &cand_n);
        if (res_n == ef && c.d > st.res[0].d) {
            break;
        }
        if (g_trace && n_hops < 40) printf("TRACE L0 hop%llu pop=(%u@%.1f) res0=%.1f res_n=%u\n",
            (unsigned long long)n_hops, c.id, c.d, st.res[0].d, res_n);
        const uint32_t *ids;
        uint32_t degree = pnm_neighbors(c.id, 0, &ids);
        n_hops++;
        for (uint32_t j = 0; j < degree; j++) {
            uint32_t nb = ids[j];
            float d = pnm_dist(nb);
            n_dist++;
            pnm_visit(&cand_n, &res_n, nb, d, ef);
        }
    }
    g_dist += n_dist;
    g_hops += n_hops;

    uint32_t m = res_n < k ? res_n : k;
    qsort(st.res, res_n, sizeof(struct pnm_cnd), pnm_cnd_asc);
    for (uint32_t i = 0; i < k; i++) {
        out[i].id = i < m ? st.res[i].id : 0xffffffffu;
        out[i].d  = i < m ? st.res[i].d : 0.0f;
    }
    return m;
}

/* ==================== window / mailbox plumbing (from pnm_client) ==================== */

uint8_t *win;
uint64_t win_blob_bytes;          /* CXL window mmap */
uint64_t win_sz;
struct pnm_mb_s *mb;   /* tail mailbox */
uint32_t g_poll_us = 0;   /* --poll-sleep: 0 = v1 tight spin */
int g_db_fd = -1;         /* doorbell mode: /dev/cylon-db */
uint32_t gen_ctr;   /* v2 state-word generation; first job sends 0 = legacy sync */
uint32_t jid;

uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void mb_reset(void)
{
    __atomic_store_n(&mb->status, PNM_MB_IDLE, __ATOMIC_RELEASE);
}

uint32_t mb_submit(uint32_t op, uint64_t a0, uint64_t a1,
                          uint32_t k, uint32_t ef, uint64_t timeout_ns,
                          struct pnm_resp_s *r)
{
    mb->job.job_id = ++jid;
    mb->job.op = op;
    mb->job.a0 = a0;
    mb->job.a1 = a1;
    mb->job.k = k;
    mb->job.ef = ef;
    mb->job.reserved0 = 0;
    mb->job.reserved1[0] = 0;
    mb->job.reserved1[1] = 0;
    /* v2: publish job + state with one u64 release-store. First job of the
     * process sends gen=0 (legacy/resync frame); the engine then requires
     * gen == last+1 (stale-PENDING replay dies). Steady state skips IDLE:
     * the next submit's single u64 store retires the previous DONE. */
    uint32_t g = gen_ctr++;
    __atomic_store_n(&mb->state, PNM_MB_PACK(g, PNM_MB_PENDING), __ATOMIC_RELEASE);

    uint64_t t0 = now_ns();
    for (;;) {
        if (g_db_fd >= 0) {
            /* doorbell arm: block in poll() until the engine MSI-X fires
             * (DONE publish + BI retrap done engine-side), then consume one
             * count. Leftover counts from earlier runs give at most one
             * spurious wakeup; the state re-check filters them. Pickup read
             * walks the retrapped trap path. */
            struct pollfd pfd = { .fd = g_db_fd, .events = POLLIN };
            uint64_t left = timeout_ns - (now_ns() - t0);
            if (!left) {
                break;              /* report via the timeout branch */
            }
            int ms = (int)(left / 1000000);
            if (ms < 1) {
                ms = 1;
            }
            int pr = poll(&pfd, 1, ms);
            if (pr > 0 && (pfd.revents & POLLIN)) {
                uint64_t cnt;
                (void)!read(g_db_fd, &cnt, sizeof(cnt));
            }
        }
        uint64_t w = __atomic_load_n(&mb->state, __ATOMIC_ACQUIRE);
        if (PNM_MB_STATE(w) == PNM_MB_DONE && PNM_MB_GEN(w) == g) {
            break;
        }
        if (now_ns() - t0 > timeout_ns) {
            fprintf(stderr, "cpu_search: job %u timeout (state %016llx)\n",
                    jid, (unsigned long long)w);
            return UINT32_MAX;
        }
        if (g_poll_us) {
            usleep(g_poll_us);
        }
    }
    *r = mb->resp;
    uint32_t rc = r->status;
    mb_reset();
    return rc;
}

/* copy a file into the window at off (scalar-first-touch per page — see
 * pnm_client.c: the KVM emulator cannot decode VEX first-touches) */
uint64_t stage_file(const char *path, uint64_t off)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cpu_search: open %s: %s\n", path, strerror(errno));
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || (uint64_t)sz + off + 4096 > win_sz) {
        fprintf(stderr, "cpu_search: %s (%ld B) does not fit at %lu\n",
                path, sz, (unsigned long)off);
        exit(2);
    }
    uint8_t *dst = win + off;
    uint64_t total = 0;
    static char buf[2 << 20];
    size_t got;
    uint64_t t0 = now_ns();
    uint64_t next_prog = 32ull << 20;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        uint64_t o = 0;
        while (o < got) {
            uint8_t *d = dst + total + o;
            uint64_t in_pg = 4096 - ((uintptr_t)d & 0xfff);
            uint64_t chunk = (uint64_t)got - o;
            if (chunk > in_pg) {
                chunk = in_pg;
            }
            if (((uintptr_t)d & 0xfff) == 0) {
                if (chunk >= 8) {
                    *(volatile uint64_t *)d = *(const uint64_t *)(buf + o);
                } else {
                    *(volatile uint8_t *)d = buf[o];
                }
            }
            memcpy(d, buf + o, (size_t)chunk);
            o += chunk;
        }
        total += got;
        if (total >= next_prog) {
            printf("  staged %lu/%ld MB (%.1f s)\n",
                   (unsigned long)(total >> 20), sz >> 20,
                   (double)(now_ns() - t0) / 1e9);
            next_prog += 32ull << 20;
        }
    }
    printf("  staged %lu/%ld MB done (%.1f s)\n",
           (unsigned long)(total >> 20), sz >> 20,
           (double)(now_ns() - t0) / 1e9);
    fclose(f);
    return total;
}

/* D2 device-initiated staging: submit PNM_OP_STAGE and poll for DONE,
 * printing coverage progress from resp.reserved (engine publishes every
 * 512-page chunk). Returns the engine status on DONE (*staged_pages =
 * coverage, pages the device staged). PNM_ST_ENOSYS = older engine.
 * Timeout does NOT fall back: the engine is still mid-job and a fallback
 * would race its final resp publish on the mailbox page. */
uint32_t stage_device(uint64_t blob_bytes, uint32_t *staged_pages)
{
    uint32_t want = (uint32_t)((blob_bytes + 4095) >> 12);

    mb->job.job_id = ++jid;
    mb->job.op = PNM_OP_STAGE;
    mb->job.a0 = blob_bytes;
    mb->job.a1 = 0;
    mb->job.k = 0;
    mb->job.ef = 0;
    mb->job.reserved0 = 0;
    mb->job.reserved1[0] = 0;
    mb->job.reserved1[1] = 0;
    uint32_t g = gen_ctr++;
    __atomic_store_n(&mb->state, PNM_MB_PACK(g, PNM_MB_PENDING), __ATOMIC_RELEASE);

    uint64_t t0 = now_ns();
    uint64_t next_print = 1000000000ull;
    uint32_t last_prog = 0;
    for (;;) {
        uint64_t w = __atomic_load_n(&mb->state, __ATOMIC_ACQUIRE);
        if (PNM_MB_STATE(w) == PNM_MB_DONE && PNM_MB_GEN(w) == g) {
            break;
        }
        uint64_t el = now_ns() - t0;
        if (el > 3600000000000ull) {   /* 1h */
            fprintf(stderr, "cpu_search: STAGE timeout (staged %u/%u pages)\n",
                    last_prog, want);
            exit(2);
        }
        if (el >= next_print) {
            last_prog = __atomic_load_n(&mb->resp.reserved, __ATOMIC_ACQUIRE);
            printf("  device staged %u/%u pages (%.1f s)\n",
                   last_prog, want, (double)el / 1e9);
            next_print += 1000000000ull;
        }
        usleep(100000);
    }
    *staged_pages = mb->resp.n_found;
    uint32_t rc = mb->resp.status;
    mb_reset();
    return rc;
}

int ping(void)
{
    struct pnm_resp_s r;
    uint32_t rc = mb_submit(PNM_OP_NOP, 0, 0, 0, 0, 5000000000ull, &r);
    if (rc != PNM_ST_OK) {
        fprintf(stderr, "cpu_search: NOP ping failed (rc=%u)\n", rc);
        return -1;
    }
    printf("engine alive (job %u)\n", r.job_id);
    return 0;
}

int flush_cache(void)
{
    struct pnm_resp_s r;
    uint32_t rc = mb_submit(PNM_OP_CACHE_FLUSH, 0, 0, 0, 0,
                            30000000000ull, &r);
    if (rc != PNM_ST_OK) {
        fprintf(stderr, "cpu_search: FLUSH failed (rc=%u)\n", rc);
        return -1;
    }
    printf("cache flushed (cold start)\n");
    return 0;
}

/* ==================== main ==================== */

int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* full-window verify vs the staged blob file (guest EPT read path).
 * Returns #pages differing. Also reports on a RE-RUN whether previously
 * bad pages are now good — the transient-vs-persistent discriminator. */
int verify_window(const char *blob)
{
    FILE *f = fopen(blob, "rb");
    if (!f) {
        return -1;
    }
    static uint8_t fbuf[2 << 20];
    uint64_t ndiff = 0, off = 0;
    uint64_t first[8];
    while (off < win_blob_bytes) {
        uint64_t chunk = win_blob_bytes - off;
        if (chunk > sizeof(fbuf)) {
            chunk = sizeof(fbuf);
        }
        if (fread(fbuf, 1, chunk, f) != chunk) {
            fclose(f);
            return -1;
        }
        for (uint64_t o = 0; o < chunk; o += 4096) {
            /* scalar byte loop: libc memcmp ifuncs to AVX2, and a VEX
             * first-touch of a trapped (evicted) page = SIGILL. Scalar
             * loads first-touch safely (KVM emulates them). */
            const volatile uint8_t *w = win + off + o;
            const uint8_t *b = fbuf + o;
            int differs = 0;
            for (uint64_t x = 0; x < 4096; x++) {
                if (w[x] != b[x]) { differs = 1; break; }
            }
            if (differs) {
                if (ndiff < 8) {
                    first[ndiff] = (off + o) >> 12;
                }
                ndiff++;
            }
        }
        off += chunk;
    }
    fclose(f);
    printf("verify-window: %lu pages differ", (unsigned long)ndiff);
    for (uint64_t i = 0; i < (ndiff < 8 ? ndiff : 8); i++) {
        printf(" [p%lu]", (unsigned long)first[i]);
    }
    printf("\n");
    return (int)(ndiff & 0x7fffffff);
}

/* ==================== collaborative mode (-F) ==================== */

/* Merged per-query results, one row per query: [0, n_cpu) filled by the
 * CPU worker pthread (local pnm_search), [n_cpu, n) by the engine feeder
 * (window results area). Slot layout mirrors the engine's results area:
 * 8B (id, dist) pairs, id 0xffffffff = empty slot. k is capped at
 * COLLAB_KMAX. */
uint32_t (*g_all_ids)[COLLAB_KMAX];
float (*g_all_d)[COLLAB_KMAX];
uint32_t *g_all_m;            /* #valid entries per query */

/* engine-side accumulators (feeder thread only) */
uint64_t g_e_dist, g_e_hops, g_e_pages, g_e_ns;


/* CPU worker: the existing local traversal over queries [0, n_cpu). The
 * feeder never touches st.*, so the shared traversal state stays
 * single-threaded (g_dist/g_hops stay CPU-side-only by construction). */
void *cpu_worker(void *arg)
{
    struct collab_ctx *c = arg;

    for (uint32_t qi = 0; qi < c->n_cpu; qi++) {
        const uint16_t *qh = (const uint16_t *)(win + c->qoff) +
                             (size_t)qi * st.dim;
        uint64_t t0 = now_ns();
        uint32_t m = pnm_search(qh, c->k, c->ef, c->out);
        c->lat[qi] = now_ns() - t0;
        g_all_m[qi] = m < c->k ? m : c->k;
        for (uint32_t j = 0; j < c->k; j++) {
            g_all_ids[qi][j] = j < m ? c->out[j].id : 0xffffffffu;
            g_all_d[qi][j] = j < m ? c->out[j].d : 0.0f;
        }
    }
    return NULL;
}

/* BIND the CYH1 blob staged at window offset 0 (exact pnm_client pattern;
 * BIND walks the whole blob, hence the generous timeout) */
int bind_index(void)
{
    struct pnm_resp_s r;
    uint32_t rc = mb_submit(PNM_OP_BIND_INDEX, 0, 0, 0, 0,
                            120000000000ull, &r);
    if (rc != PNM_ST_OK) {
        fprintf(stderr, "cpu_search: BIND failed (rc=%u)\n", rc);
        return -1;
    }
    printf("index bound (job %u)\n", r.job_id);
    return 0;
}

/* Engine feeder: one outstanding SEARCH job at a time over [n_cpu, n),
 * results parsed straight from the window results area (pnm_client
 * pattern). Errors are fatal: caller returns 1. */
int engine_feeder(uint64_t qoff, uint32_t n, uint32_t n_cpu,
                         uint32_t k, uint32_t ef, uint64_t *lat)
{
    const uint64_t res_off = win_sz - PNM_RESULTS_OFF_FROM_END;

    for (uint32_t qi = n_cpu; qi < n; qi++) {
        struct pnm_resp_s r;
        uint64_t a0 = qoff + (uint64_t)qi * st.dim * 2;
        uint64_t t0 = now_ns();
        uint32_t rc = mb_submit(PNM_OP_ANNS_SEARCH, a0, res_off, k, ef,
                                10000000000ull, &r);
        lat[qi] = now_ns() - t0;
        if (rc == UINT32_MAX) {
            fprintf(stderr, "cpu_search: engine search timeout at query %u\n",
                    qi);
            return -1;
        }
        if (rc != PNM_ST_OK) {
            fprintf(stderr, "cpu_search: engine search %u failed (rc=%u)\n",
                    qi, rc);
            return -1;
        }
        const uint8_t *rp = win + res_off;
        uint32_t n_found;
        memcpy(&n_found, rp + (size_t)k * 8, 4);
        if (n_found > k) {
            n_found = k;
        }
        g_all_m[qi] = n_found;
        for (uint32_t j = 0; j < k; j++) {
            memcpy(&g_all_ids[qi][j], rp + (size_t)j * 8, 4);
            memcpy(&g_all_d[qi][j], rp + (size_t)j * 8 + 4, 4);
        }
        /* normalize slots beyond n_found to the empty-slot convention */
        for (uint32_t j = n_found; j < k; j++) {
            g_all_ids[qi][j] = 0xffffffffu;
            g_all_d[qi][j] = 0.0f;
        }
        g_e_dist += r.n_dist;
        g_e_hops += r.n_hops;
        g_e_pages += r.n_pages;
        g_e_ns += r.total_ns;
    }
    return 0;
}
