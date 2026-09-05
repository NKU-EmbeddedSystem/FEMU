/*
 * Cylon PNM engine — ANNS (HNSW) offload engine thread for CXLSSD.
 *
 * Plain QEMU thread (no BQL), same discipline as the FTL thread. Consumes
 * jobs from a mailbox at the tail of the CXL window (pnm_uapi.h). Query,
 * result and mailbox accesses go through cache-aware xlate (hit -> pmem
 * cache slice, miss -> logical_space), the same locations the guest's
 * hardware path uses, so results are coherent with the guest view.
 *
 * Graph reads (vectors / adjacency) have two modes (pnm_use_local_copy):
 *  - cache-aware (default): every 4K page goes through the cache hierarchy;
 *    a miss is a NAND page read — the page is filled into the cache (policy
 *    insert + eviction, same path the guest FTL takes) and the job is
 *    charged pg_rd_lat. bufsz/policy/prefetch therefore shape search QPS.
 *  - local copy (PNM_LOCAL_COPY=1, or forced by cxl_skip_ftl acceptance
 *    mode): the blob is copied once at BIND into engine-local memory.
 *    Phase-A correctness path, bypasses the cache hierarchy entirely.
 */
#include "../nvme.h"
#include "../ftl/ftl.h"
#include "cxlssd.h"
#include "pnm_uapi.h"
#include "der_kvm.h"
#include "cache/cache.h"
#include "cache/cache_plugin.h"
#include "qemu/thread.h"
#include "qemu/timer.h"

/* ---------------- fp16 -> fp32 (software, exact) ---------------- */
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
            /* subnormal: value = man * 2^-24; renormalize */
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

/* ---------------- binary heaps over {float,u32} ---------------- */
struct pnm_cnd {
    float d;
    uint32_t id;
};

static inline void heap_swap(struct pnm_cnd *a, uint32_t i, uint32_t j)
{
    struct pnm_cnd t = a[i];
    a[i] = a[j];
    a[j] = t;
}

/* min-heap by d (candidate heap) */
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

/* max-heap by d (result heap: root = worst of the best) */
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

/* ---------------- PNM engine state ---------------- */
struct pnm_state {
    Cxlssd *ctx;
    FemuCtrl *n;
    volatile int stop;
    QemuThread thread;
    bool started;

    /* bound CYH1 index metadata */
    bool bound;
    uint64_t idx_base;
    uint32_t dim, count, maxm0;
    uint32_t entry_point;
    uint8_t entry_level;
    uint64_t g_off_vectors, g_off_adj0, g_off_upper, g_off_levels;

    /* engine-local graph copy (copied from window at BIND); NULL in
     * cache-aware mode (search reads the window through the cache) */
    uint8_t *graph;
    bool local_copy;            /* Phase-A local-copy search (env/acceptance) */
    uint8_t *adj_buf;           /* cache-aware: one neighbor record scratch */

    /* per-job scratch (allocated at BIND) */
    uint8_t *visited;            /* bitset, count bits */
    struct pnm_cnd *cand;        /* candidate min-heap, 64K entries */
    struct pnm_cnd *res;         /* result max-heap, cap 4K */
    uint32_t cand_cap, res_cap;
    float *qconv;                /* query converted to fp32, [dim] */

    /* stats of the last completed job (copied into the mailbox resp) */
    uint64_t job_ns, job_dist, job_hops, job_pages;
    uint64_t job_misses, job_miss_ns;   /* search-time cache misses + charge */
    uint32_t job_found;
};

/* cache-aware translation: hit -> pmem cache slice, miss -> logical_space */
static void *pnm_xlate(Cxlssd *ctx, FemuCtrl *n, uint64_t off)
{
    if (ctx->cache) {
        /* locked, by-value slot lookup: safe against concurrent FTL-thread
         * inserts/evictions (GTree is not thread-safe, and a returned
         * CacheEntry* could be freed by an eviction) */
        uint32_t slot = cylon_cache_lookup_slot(ctx->cache->cache_data,
                                                off >> 12);
        if (slot != UINT32_MAX) {
            return (char *)ctx->cache_backend.buf_space +
                   (size_t)slot * CACHE_PAGE_SIZE + (off & 0xfff);
        }
    }
    return (char *)n->mbe->logical_space + off;
}

/* xlate-aware reader (handles 4K straddling) */
static void pnm_read(Cxlssd *ctx, FemuCtrl *n, uint64_t off, void *dst, uint32_t len)
{
    uint8_t *d = dst;
    while (len) {
        uint32_t in_page = 4096 - (off & 0xfff);
        uint32_t take = len < in_page ? len : in_page;
        memcpy(d, pnm_xlate(ctx, n, off), take);
        d += take;
        off += take;
        len -= take;
    }
}

/* xlate-aware writer */
static void pnm_write(Cxlssd *ctx, FemuCtrl *n, uint64_t off, const void *src, uint32_t len)
{
    const uint8_t *s = src;
    while (len) {
        uint32_t in_page = 4096 - (off & 0xfff);
        uint32_t take = len < in_page ? len : in_page;
        memcpy(pnm_xlate(ctx, n, off), s, take);
        s += take;
        off += take;
        len -= take;
    }
}

/* Search data-path mode. PNM_LOCAL_COPY=1 forces the Phase-A engine-local
 * graph copy (also forced in cxl_skip_ftl acceptance mode, where nothing
 * populates the cache and every read would be a charged miss). Default:
 * cache-aware reads through the cache/NAND hierarchy, so bufsz/policy/
 * prefetch actually shape search latency. */
static bool pnm_use_local_copy(FemuCtrl *n)
{
    static int mode = -1;
    if (mode < 0) {
        const char *s = getenv("PNM_LOCAL_COPY");
        mode = (s && *s && *s != '0') ? 1 : 0;
    }
    return mode || n->cxl_skip_ftl;
}

/* Cache-aware search-time graph read: hit -> pmem slot (no charge); miss ->
 * the device pulls the page from NAND into the cache (the full insert path:
 * policy insert + eviction, exactly what the guest FTL path does) and the
 * job is charged one NAND page read (pg_rd_lat). off is a window offset. */
static void pnm_graph_read(struct pnm_state *st, uint64_t off, void *dst, uint32_t len)
{
    Cxlssd *ctx = st->ctx;
    FemuCtrl *n = st->n;
    uint8_t *d = dst;

    while (len) {
        uint32_t in_page = 4096 - (off & 0xfff);
        uint32_t take = len < in_page ? len : in_page;
        lpn_t lpn = off >> 12;
        uint32_t slot = UINT32_MAX;

        /* A read past the window end means a corrupt index/neighbor id
         * (observed: slot-aliasing corruption served vector bits as an
         * adjacency list -> id 0x51E04A00 -> off ~327GB -> SIGSEGV on the
         * logical_space fallback). Zero-fill and say so instead of dying;
         * the dump gate catches the bad query. */
        if (off + take > (uint64_t)n->mbe->size) {
            static unsigned warned;
            if (warned++ < 8) {
                femu_err("Cylon PNM: !! graph read past window end "
                         "(off 0x%llx len %u) — corrupt index data\n",
                         (unsigned long long)off, len);
            }
            memset(d, 0, len);
            return;
        }

        if (ctx->cache) {
            slot = cylon_cache_lookup_slot(ctx->cache->cache_data, lpn);
            if (slot == UINT32_MAX) {
                struct cache_plugin *cp = ctx->cache;
                struct cache_entry *e = cp->ops.entry_init(cp->cache_data, lpn);
                cp->ops.insert(cp->cache_data, e, 0, false);
                slot = cylon_cache_lookup_slot(cp->cache_data, lpn);
                st->job_misses++;
                st->job_miss_ns += (uint64_t)n->bb_params.pg_rd_lat;
            }
        }
        memcpy(d,
               slot != UINT32_MAX
                   ? (uint8_t *)ctx->cache_backend.buf_space +
                         (size_t)slot * CACHE_PAGE_SIZE + (off & 0xfff)
                   : (uint8_t *)n->mbe->logical_space + off,
               take);
        d += take;
        off += take;
        len -= take;
    }
}

/* drop any previously bound index (re-BIND or shutdown) */
static void pnm_unbind(struct pnm_state *st)
{
    g_free(st->graph);
    g_free(st->visited);
    g_free(st->adj_buf);
    st->graph = NULL;
    st->visited = NULL;
    st->adj_buf = NULL;
    st->bound = false;
}

/* FLUSH: cold-start the device cache between experiments (re-run support);
 * see cylon_cache_reset() for what and why */
static int pnm_handle_flush(struct pnm_state *st)
{
    Cxlssd *ctx = st->ctx;

    if (ctx->cache && ctx->cache->cache_data) {
        cylon_cache_reset((Cache *)ctx->cache->cache_data);
    }
    return PNM_ST_OK;
}

/* ---------------- job handlers ---------------- */

static int pnm_handle_bind(struct pnm_state *st, uint64_t a0)
{
    Cxlssd *ctx = st->ctx;
    FemuCtrl *n = st->n;
    pnm_unbind(st);
    struct cyh1_header hdr;
    uint64_t win_sz = (uint64_t)n->mbe->size;
    uint64_t blob_sz;

    pnm_read(ctx, n, a0, &hdr, sizeof(hdr));

    if (hdr.magic != CYH1_MAGIC || hdr.version != CYH1_VERSION) {
        femu_log("Cylon PNM: BIND bad magic/version (0x%08x/%u)\n",
                 hdr.magic, hdr.version);
        return PNM_ST_EINVAL;
    }
    if (!hdr.count || hdr.count > (1u << 26) || !hdr.dim || hdr.dim > 4096) {
        femu_log("Cylon PNM: BIND bad count/dim (%u/%u)\n", hdr.count, hdr.dim);
        return PNM_ST_EINVAL;
    }
    if (hdr.maxm0 > 128) {
        return PNM_ST_EINVAL;
    }

    blob_sz = hdr.off_levels + hdr.count;   /* levels array is the last section */
    if (a0 + blob_sz > win_sz - PNM_MB_OFF_FROM_END) {
        femu_log("Cylon P engine: BIND blob overflows window (blob_end %" PRIu64
                 " > limit %" PRIu64 ")\n", a0 + blob_sz,
                 win_sz - PNM_MB_OFF_FROM_END);
        return PNM_ST_EINVAL;
    }

    st->local_copy = pnm_use_local_copy(n);
    st->idx_base = a0;      /* window offset of blob start (cache-aware reads) */

    /* engine-local copy of the whole blob (read-only, race-free) */
    if (st->local_copy) {
        uint8_t *graph = g_malloc0(blob_sz);
        if (!graph) {
            return PNM_ST_ENOMEM;
        }
        for (uint64_t i = 0; i < blob_sz; i += 4096) {
            uint32_t chunk = blob_sz - i > 4096 ? 4096 : (uint32_t)(blob_sz - i);
            pnm_read(ctx, n, a0 + i, graph + i, chunk);
        }
        st->graph = graph;
    } else {
        /* cache-aware search reads the window page by page (miss -> NAND
         * fill + charge); one scratch record buffer instead of the blob */
        st->adj_buf = g_malloc0(4 + (size_t)hdr.maxm0 * 4);
        if (!st->adj_buf) {
            return PNM_ST_ENOMEM;
        }
    }

    st->dim = hdr.dim;
    st->count = hdr.count;
    st->maxm0 = hdr.maxm0;
    st->entry_point = hdr.entry_point;
    st->entry_level = (uint8_t)hdr.entry_level;
    st->g_off_vectors = hdr.off_vectors;
    st->g_off_adj0 = hdr.off_adj0;
    st->g_off_upper = hdr.off_upper;
    st->g_off_levels = hdr.off_levels;
    /* visited must be sized from *this* bind's count (the stale-count alloc
     * used to hand search a 0-byte buffer -> memset NULL -> SIGSEGV) */
    st->visited = g_malloc0((st->count + 7) / 8);
    st->bound = true;

    femu_log("Cylon PNM: index bound (%s search, dim %u count %u maxm0 %u blob %" PRIu64 " MB)\n",
             st->local_copy ? "local-copy" : "cache-aware",
             st->dim, st->count, st->maxm0, blob_sz >> 20);
    return PNM_ST_OK;
}

/* ---------------- HNSW traversal ---------------- */

/* L2^2 distance of query (st->qconv) vs vector id.
 * local_copy: direct pointer into the engine-local blob.
 * cache-aware: one pnm_graph_read (hit -> slot, miss -> fill + charge). */
static inline float pnm_dist(struct pnm_state *st, uint32_t id)
{
    const float *q = st->qconv;
    float acc = 0.0f;

    if (st->local_copy) {
        const uint16_t *v = (const uint16_t *)(st->graph + st->g_off_vectors) +
                            (size_t)id * st->dim;
        for (uint32_t i = 0; i < st->dim; i++) {
            float diff = q[i] - pnm_f16_to_f32(v[i]);
            acc += diff * diff;
        }
        return acc;
    }
    uint16_t v[4096];
    pnm_graph_read(st, st->idx_base + st->g_off_vectors +
                        (uint64_t)id * st->dim * 2, v, st->dim * 2);
    for (uint32_t i = 0; i < st->dim; i++) {
        float diff = q[i] - pnm_f16_to_f32(v[i]);
        acc += diff * diff;
    }
    return acc;
}

/* neighbors at a level: 0 -> adj0 fixed stride; >=1 -> upper records.
 * *ids stays valid until the next pnm_neighbors call (cache-aware mode
 * points into the per-job adj_buf scratch, overwritten next call). */
static uint32_t pnm_neighbors(struct pnm_state *st, uint32_t id, int level,
                              const uint32_t **ids)
{
    if (st->local_copy) {
        if (level == 0) {
            const uint32_t *a = (const uint32_t *)(st->graph + st->g_off_adj0) +
                                (size_t)id * st->maxm0;
            *ids = a;
            uint32_t degree = 0;
            while (degree < st->maxm0 && a[degree] != 0xffffffffu) {
                degree++;
            }
            return degree;
        }
        uint32_t off = ((const uint32_t *)(st->graph + st->g_off_upper))[id];
        if (!off) {
            *ids = NULL;
            return 0;
        }
        /* record offsets are relative to the upper section start (the offset
         * table itself); 0 = element has no upper levels */
        const uint8_t *r = (const uint8_t *)st->graph + st->g_off_upper + off;
        for (int l = 1; l < level; l++) {
            uint32_t d = *(const uint32_t *)r;
            (void)d;
            r += 4 + d * 4;
        }
        *ids = (const uint32_t *)(r + 4);
        return *(const uint32_t *)r;
    }

    /* cache-aware: pull the record through the cache hierarchy */
    uint64_t base = st->idx_base;
    if (level == 0) {
        pnm_graph_read(st, base + st->g_off_adj0 + (uint64_t)id * st->maxm0 * 4,
                       st->adj_buf, st->maxm0 * 4);
        const uint32_t *a = (const uint32_t *)st->adj_buf;
        *ids = a;
        uint32_t degree = 0;
        while (degree < st->maxm0 && a[degree] != 0xffffffffu) {
            degree++;
        }
        return degree;
    }
    uint32_t off;
    pnm_graph_read(st, base + st->g_off_upper + (uint64_t)id * 4, &off, 4);
    if (!off) {
        *ids = NULL;
        return 0;
    }
    uint64_t rec = base + st->g_off_upper + off;
    for (int l = 1; l < level; l++) {
        uint32_t d;
        pnm_graph_read(st, rec, &d, 4);
        rec += 4 + (uint64_t)d * 4;
    }
    uint32_t degree;
    pnm_graph_read(st, rec, &degree, 4);
    if (degree > st->maxm0) {
        degree = st->maxm0;     /* malformed record: clamp to scratch size */
    }
    pnm_graph_read(st, rec + 4, st->adj_buf, degree * 4);
    *ids = (const uint32_t *)st->adj_buf;
    return degree;
}

/* level-0 visit: mark visited, push into candidate + result heaps */
static bool pnm_visit(struct pnm_state *st, uint32_t *cand_n, uint32_t *res_n,
                      uint32_t id, float d, uint32_t ef)
{
    if (st->visited[id >> 3] & (1u << (id & 7))) {
        return false;
    }
    st->visited[id >> 3] |= 1u << (id & 7);

    if (*res_n < ef) {
        heap_push_max(st->res, res_n, st->res_cap, d, id);
    } else if (d < st->res[0].d) {
        heap_pop_max(st->res, res_n);
        heap_push_max(st->res, res_n, st->res_cap, d, id);
    } else {
        return false;
    }
    heap_push_min(st->cand, cand_n, st->cand_cap, d, id);
    return true;
}

static int pnm_cmp_asc(const void *a, const void *b)
{
    const struct pnm_cnd *ca = a, *cb = b;
    if (ca->d == cb->d) {
        /* canonical tie-break by id: with ~14% of queries carrying an
         * equal-distance pair in the top-10, a bare qsort here makes the
         * dump a coin-flip on heap-array order across code paths */
        return ca->id < cb->id ? -1 : (ca->id > cb->id ? 1 : 0);
    }
    return ca->d < cb->d ? -1 : 1;
}

static int pnm_handle_search(struct pnm_state *st, uint32_t job_id,
                             uint64_t a0, uint64_t a1, uint32_t k, uint32_t ef)
{
    Cxlssd *ctx = st->ctx;
    FemuCtrl *n = st->n;
    uint64_t win_sz = (uint64_t)n->mbe->size;

    if (!st->bound) {
        return PNM_ST_ENOINDEX;
    }
    if (!k || k > st->res_cap || ef < k || ef > 4096) {
        return PNM_ST_EINVAL;
    }
    if (a0 + (uint64_t)st->dim * 2 > win_sz ||
        a1 + (uint64_t)k * 8 + 4 > win_sz) {
        return PNM_ST_EINVAL;
    }

    /* load + convert query */
    {
        uint16_t qh[4096] = {0};
        pnm_read(ctx, n, a0, qh, st->dim * 2);
        for (uint32_t i = 0; i < st->dim; i++) {
            st->qconv[i] = pnm_f16_to_f32(qh[i]);
        }
    }

    uint64_t n_dist = 1, n_hops = 0;    /* entry-point distance counted */
    st->job_misses = 0;
    st->job_miss_ns = 0;
    uint64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    /* greedy descent through upper levels (entry -> level 1) */
    uint32_t cur = st->entry_point;
    float cur_d = pnm_dist(st, cur);
    for (int l = st->entry_level; l >= 1; l--) {
        bool improved = true;
        while (improved) {
            improved = false;
            const uint32_t *ids;
            uint32_t degree = pnm_neighbors(st, cur, l, &ids);
            n_hops++;
            for (uint32_t j = 0; j < degree; j++) {
                uint32_t nb = ids[j];
                float d = pnm_dist(st, nb);
                n_dist++;
                if (d < cur_d) {
                    cur_d = d;
                    cur = nb;
                    improved = true;
                }
            }
        }
    }

    /* level-0 ef-search */
    uint32_t cand_n = 0, res_n = 0;
    memset(st->visited, 0, (st->count + 7) / 8);

    pnm_visit(st, &cand_n, &res_n, cur, cur_d, ef);

    while (cand_n) {
        struct pnm_cnd c = heap_pop_min(st->cand, &cand_n);
        if (res_n == ef && c.d > st->res[0].d) {
            break;
        }
        const uint32_t *ids;
        uint32_t degree = pnm_neighbors(st, c.id, 0, &ids);
        n_hops++;
        for (uint32_t j = 0; j < degree; j++) {
            uint32_t nb = ids[j];
            float d = pnm_dist(st, nb);
            n_dist++;
            pnm_visit(st, &cand_n, &res_n, nb, d, ef);
        }
    }

    /* modeled device compute capability: the PNM dist array charges
     * compute-ns per distance, settled as a busy-wait at job end (the
     * "compute array beside the cache, controller core free" model;
     * serial-adds to the miss burn below, the conservative non-overlapped
     * model). Read from /tmp/femu-compute-ns per job so calibration sweeps
     * tune it live without a restart; absent/0 = unthrottled (the engine
     * that produced all E1/E2 baselines). Real CXL SSD controllers
     * (~1-2 ARM cores) sit at ~1-10us/dist = 1/4-1/10 of a server core,
     * so the sweep anchors the device-compute axis for the paper. */
    uint64_t comp_ns = 0;
    FILE *cf = fopen("/tmp/femu-compute-ns", "r");
    if (cf) {
        char cb[32] = {0};
        if (fgets(cb, sizeof(cb), cf)) {
            comp_ns = strtoull(cb, 0, 0);
        }
        fclose(cf);
    }
    if (comp_ns) {
        uint64_t ctarget = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
                           n_dist * comp_ns;
        while (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) < ctarget) {
        }
    }

    /* modeled NAND service time for search-time page misses: burn it for
     * real (busy-wait, ns precision) so the job's wall clock — and the
     * client's QPS — reflects the cache hierarchy, not just host speed.
     * Serial (non-overlapped) miss latency, the conservative model. */
    if (st->job_miss_ns) {
        uint64_t target = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + st->job_miss_ns;
        while (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) < target) {
            /* spin */
        }
    }
    uint64_t t1 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    st->job_ns = t1 - t0;
    st->job_dist = n_dist;
    st->job_hops = n_hops;
    st->job_pages = st->job_misses;

    /* top-k: sort ascending by distance, emit k entries {u32 id, f32 dist} */
    uint32_t m = res_n < k ? res_n : k;
    qsort(st->res, res_n, sizeof(struct pnm_cnd), pnm_cmp_asc);
    for (uint32_t i = 0; i < k; i++) {
        uint32_t id = i < m ? st->res[i].id : 0xffffffffu;
        float d = i < m ? st->res[i].d : 0.0f;
        pnm_write(ctx, n, a1 + (uint64_t)i * 8, &id, 4);
        pnm_write(ctx, n, a1 + (uint64_t)i * 8 + 4, &d, 4);
    }
    pnm_write(ctx, n, a1 + (uint64_t)k * 8, &m, 4);

    st->job_found = m;
    femu_log("Cylon PNM: search job %u k=%u ef=%u -> %u found (%" PRIu64
             " ns, dist %" PRIu64 ", hops %" PRIu64 ", misses %" PRIu64 ")\n",
             job_id, k, ef, m, st->job_ns, n_dist, n_hops, st->job_misses);
    return PNM_ST_OK;
}

/* ---------------- mailbox polling thread ---------------- */

static void *pnm_thread_fn(void *opaque)
{
    struct pnm_state *st = opaque;
    Cxlssd *ctx = st->ctx;
    FemuCtrl *n = st->n;
    const uint64_t mb_off = (uint64_t)n->mbe->size - PNM_MB_OFF_FROM_END;

    femu_log("Cylon PNM: engine up (mailbox at window_end-0x%" PRIx64 ")\n",
             (uint64_t)PNM_MB_OFF_FROM_END);

    while (!st->stop) {
        /* status sits at a higher offset than the job fields, so copying the
         * whole mailbox in one ascending pass can observe a fresh PENDING
         * with stale job fields (the field loads execute before the writer's
         * field stores become globally visible). Read the flag FIRST, then
         * the payload: on x86 the later field loads are then guaranteed to
         * see the stores that became visible before PENDING (TSO keeps store
         * order). Without this, ~1% of jobs run the previous job's a0. */
        uint32_t status;
        pnm_read(ctx, n, mb_off + offsetof(struct pnm_mb_s, status),
                 &status, sizeof(status));
        {
            static unsigned dbg_n;
            dbg_n++;
            if (femu_cxldbg_on() && (dbg_n <= 16 || (dbg_n % 262144) == 0)) {
                const char *lraw = (const char *)n->mbe->logical_space + mb_off;
                uint64_t *e4k = der_kvm_get_eptep_dbg(ctx, 12582911);
                uint64_t *e2m = der_kvm_get_eptep_dbg(ctx, 12321279);
                fprintf(stderr,
                        "CXLDBG PNM poll #%u status=%u log=%02x%02x%02x%02x %02x%02x%02x%02x e4k=%016" PRIx64 " e2m=%016" PRIx64 "\n",
                        dbg_n, status,
                        lraw[0], lraw[1], lraw[2], lraw[3],
                        lraw[4], lraw[5], lraw[6], lraw[7],
                        e4k ? (uint64_t)*e4k : (uint64_t)0,
                        e2m ? (uint64_t)*e2m : (uint64_t)0);
            }
        }
        if (status != PNM_MB_PENDING) {
            g_usleep(50);
            continue;
        }
        struct pnm_mb_s mb;
        pnm_read(ctx, n, mb_off, &mb, sizeof(mb));
        if (mb.status != PNM_MB_PENDING) {
            g_usleep(50);
            continue;
        }

        /* defaults; SEARCH overrides */
        st->job_found = 0;
        st->job_ns = 0;
        st->job_dist = 0;
        st->job_hops = 0;
        st->job_pages = 0;

        int rc;
        switch (mb.job.op) {
        case PNM_OP_NOP:
            rc = PNM_ST_OK;
            break;
        case PNM_OP_BIND_INDEX:
            rc = pnm_handle_bind(st, mb.job.a0);
            femu_log("Cylon PNM: BIND job %u -> %d\n", mb.job.job_id, rc);
            break;
        case PNM_OP_ANNS_SEARCH:
            rc = pnm_handle_search(st, mb.job.job_id, mb.job.a0, mb.job.a1,
                                   mb.job.k, mb.job.ef);
            break;
        case PNM_OP_CACHE_FLUSH:
            rc = pnm_handle_flush(st);
            femu_log("Cylon PNM: FLUSH job %u -> %d\n", mb.job.job_id, rc);
            break;
        default:
            femu_log("Cylon PNM: job %u unknown op %u\n", mb.job.job_id,
                     mb.job.op);
            rc = PNM_ST_ENOSYS;
            break;
        }

        mb.resp.job_id = mb.job.job_id;
        mb.resp.status = (uint32_t)rc;
        mb.resp.n_found = st->job_found;
        mb.resp.reserved = 0;
        mb.resp.total_ns = st->job_ns;
        mb.resp.n_dist = st->job_dist;
        mb.resp.n_hops = st->job_hops;
        mb.resp.n_pages = st->job_pages;

        pnm_write(ctx, n, mb_off + offsetof(struct pnm_mb_s, resp),
                  &mb.resp, sizeof(mb.resp));
        __atomic_thread_fence(__ATOMIC_RELEASE);
        uint32_t done = PNM_MB_DONE;
        pnm_write(ctx, n, mb_off + offsetof(struct pnm_mb_s, status),
                  &done, sizeof(done));

        /* D1 Type-2 BI bill: flip the mailbox page (and the results page
         * the client reads) back to trap so the client's next pickup read
         * exits into FEMU, where the trap path charges the
         * snoop-equivalent latency before flipping direct again. Knob is
         * re-read per job (same live-tune pattern as the compute knob);
         * 0 = off = E1'-identical behavior. a1=0 ops (NOP/FLUSH) skip the
         * results-page retrap (not a tail lpn -> no-op). */
        {
            uint64_t bi_ns = 0;
            FILE *bf = fopen("/tmp/femu-bi-lat-ns", "r");
            if (bf) {
                char bb[32] = { 0 };
                if (fgets(bb, sizeof(bb), bf)) {
                    bi_ns = strtoull(bb, 0, 0);
                }
                fclose(bf);
            }
            cylon_bi_lat_ns = bi_ns;
            if (bi_ns) {
                der_kvm_epte_retrap_control(ctx, mb_off >> 12);
                der_kvm_epte_retrap_control(ctx, mb.job.a1 >> 12);
                femu_log("Cylon PNM: BI re-trap job %u (knob %lu ns)\n",
                         mb.job.job_id, (unsigned long)bi_ns);
            }
        }
        /* readback probe: the invariant is that the mailbox page NEVER
         * lives in a cache slot (it is tail-pinned direct); rb == done is
         * racy by design — the client resets the mailbox for the next job
         * the moment it sees DONE, so it is not part of the check */
        {
            uint32_t slot = ctx->cache
                ? cylon_cache_lookup_slot(ctx->cache->cache_data,
                                          (mb_off >> 12))
                : UINT32_MAX;
            if (slot != UINT32_MAX) {
                static unsigned warned;
                if (warned++ < 8) {
                    femu_err("Cylon PNM: !! mailbox lpn in slot %u, job %u\n",
                             slot, mb.job.job_id);
                }
            }
        }
    }
    return NULL;
}

/* ---------------- lifecycle ---------------- */

void pnm_start(Cxlssd *ctx, struct FemuCtrl *n)
{
    struct pnm_state *st = g_malloc0(sizeof(*st));

    st->ctx = ctx;
    st->n = n;
    st->cand_cap = 65536;
    st->res_cap = 4096;
    st->cand = g_malloc0(sizeof(struct pnm_cnd) * (size_t)st->cand_cap);
    st->res = g_malloc0(sizeof(struct pnm_cnd) * (size_t)st->res_cap);
    st->qconv = g_malloc0(sizeof(float) * 4096);
    ctx->pnm = st;

    qemu_thread_create(&st->thread, "cylon-pnm", pnm_thread_fn, st,
                       QEMU_THREAD_JOINABLE);
    st->started = true;
    femu_log("Cylon PNM: engine started\n");
}

void pnm_stop(Cxlssd *ctx)
{
    struct pnm_state *st = ctx->pnm;

    if (!st) {
        return;
    }
    ctx->pnm = NULL;
    if (st->started) {
        st->stop = 1;
        qemu_thread_join(&st->thread);
    }
    pnm_unbind(st);
    g_free(st->cand);
    g_free(st->res);
    g_free(st->qconv);
    g_free(st);
}
