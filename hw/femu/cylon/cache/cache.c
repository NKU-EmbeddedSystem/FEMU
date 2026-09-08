#include "cache.h"
#include "cache_impl.h"
#include "cache_plugin.h"
#include "policy/cache_policy.h"
#include "cache_backend.h"
#include "hw/femu/femu.h"
#include "../../ftl/ftl.h"
#include "../cxlssd.h"
#include "../der_kvm.h"
#include <glib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>

static int lpn_cmp(const void *a, const void *b)
{
    const CacheEntry *ea = a;
    const CacheEntry *eb = b;
    if (ea->lpn < eb->lpn) return -1;
    if (ea->lpn > eb->lpn) return 1;
    return 0;
}

/*
 * Real HPA of a page in our own address space, via /proc/self/pagemap.
 * Used to sanity/derive the cache backend's HPA base: the run script's
 * cache_hpa_base is the pmem REGION base, but in pfn ("memory") namespace
 * mode the block device's data area starts at region base + data offset
 * (>= 2MB) — so the param can point outside the mmap and guest direct
 * EPTEs would target the reserved metadata area. Translating our own mmap
 * is offset-proof. Returns 0 if not resolvable (needs CAP_SYS_ADMIN for
 * the PFN; FEMU runs as root). QEMU is single-threaded here, no locking.
 */
static uint64_t cache_pagemap_hpa(const void *page)
{
    uint64_t ent = 0;
    FILE *f = fopen("/proc/self/pagemap", "rb");
    if (!f) {
        return 0;
    }
    if (fseek(f, (long)(((uintptr_t)page >> 12) * 8), SEEK_SET) == 0 &&
        fread(&ent, sizeof(ent), 1, f) == 1 && (ent & (1ULL << 63))) {
        fclose(f);
        return (ent & ((1ULL << 55) - 1)) << 12;
    }
    fclose(f);
    return 0;
}

int cylon_cache_backend_init(FemuCtrl *n, CylonCacheBackend *out)
{
    int fd;
    int64_t csize;
    void *p;

    out->buf_space = NULL;
    out->buf_size = 0;
    out->hpa_base = 0;

    if (!n->mbe) {
        return 0;
    }
    out->buf_space = n->mbe->logical_space;
    out->buf_size = n->bufsz ? (int64_t)n->bufsz * 1024 * 1024 : 0;

    if (!n->cache_backend_dev || !n->bufsz) {
        return 0;
    }

    csize = (int64_t)n->bufsz * 1024 * 1024;
    fd = open(n->cache_backend_dev, O_RDWR);
    if (fd < 0) {
        femu_err("Failed to open cache backend device %s: %s\n",
                 n->cache_backend_dev, strerror(errno));
        return -1;
    }

    p = mmap(NULL, (size_t)csize, PROT_READ | PROT_WRITE, MAP_SHARED,
             fd, (off_t)n->cache_bdev_offset);
    close(fd);
    if (p == MAP_FAILED) {
        femu_err("Failed to mmap cache backend %s: %s\n",
                 n->cache_backend_dev, strerror(errno));
        return -1;
    }

    if (mlock(p, (size_t)csize) != 0) {
        munmap(p, (size_t)csize);
        femu_err("Failed to mlock cache backend\n");
        return -1;
    }

    out->buf_space = p;
    out->buf_size = csize;

    /* Resolve the HPA base of the backend mapping. Any blockdev/file mmap
     * (fsdax, raw, regular file) is backed by PAGE CACHE — reclaimable,
     * writeback-write-protected pages: the DER guest-direct EPTEs must
     * never point there (we saw a 1.7M/s write-fault loop on a dirty
     * page-cache page). The backend therefore must be devdax
     * (/dev/daxN.M, mmap = one-shot remap_pfn_range, VM_PFNMAP, no page
     * cache). For devdax the sysfs resource file is the authoritative
     * physical base of the mapping; pagemap may not expose PFNs for
     * VM_PFNMAP entries, so keep it only as a cross-check, and the
     * cache_hpa_base param as last resort. */
    {
        const char *dev = n->cache_backend_dev;
        const char *bn = strrchr(dev, '/');
        bn = bn ? bn + 1 : dev;
        uint64_t derived = 0;
        uint64_t res = 0;

        if (!strncmp(dev, "/dev/dax", 8)) {
            char sysfs[256], line[64];
            FILE *sf;
            snprintf(sysfs, sizeof(sysfs),
                     "/sys/bus/dax/devices/%s/resource", bn);
            sf = fopen(sysfs, "r");
            if (sf) {
                /* the file holds "0x..." — %llu stops at the 'x', so
                 * parse the line with strtoull (base 0 autodetects) */
                if (fgets(line, sizeof(line), sf)) {
                    res = (uint64_t)strtoull(line, NULL, 0);
                }
                fclose(sf);
            }
            if (!res) {
                femu_err("Cylon DER cache: devdax %s: no sysfs resource "
                         "(falling back to pagemap/param)\n", bn);
            }
        }

        derived = cache_pagemap_hpa(p);
        if (res) {
            out->hpa_base = res + (uint64_t)n->cache_bdev_offset;
            if (derived && derived != out->hpa_base) {
                femu_err("Cylon DER cache: hpa sysfs 0x%" PRIx64
                         " != pagemap 0x%" PRIx64 " (sysfs wins)\n",
                         out->hpa_base, derived);
            }
        } else if (derived) {
            out->hpa_base = derived;
            if (derived != n->cache_hpa_base) {
                femu_err("Cylon DER cache: hpa derived via pagemap 0x%" PRIx64
                         " != param 0x%" PRIx64 " (param IGNORED — likely pfn "
                         "namespace data offset)\n",
                         derived, (uint64_t)n->cache_hpa_base);
            }
        } else {
            out->hpa_base = n->cache_hpa_base;
        }
    }

    femu_log("Cylon DER cache backend: %s offset 0x%" PRIx64 " size %" PRId64 " MB, hpa_base 0x%" PRIx64 "\n",
             n->cache_backend_dev, (uint64_t)n->cache_bdev_offset, (int64_t)n->bufsz, out->hpa_base);
    return 0;
}

void cylon_cache_backend_fini(CylonCacheBackend *b)
{
    if (b && b->buf_space && b->buf_size > 0) {
        void *p = b->buf_space;
        size_t sz = (size_t)b->buf_size;
        munlock(p, sz);
        munmap(p, sz);
    }
    if (b) {
        b->buf_space = NULL;
        b->buf_size = 0;
        b->hpa_base = 0;
    }
}

/* ---- Core cache API (Cache *, CacheEntry *) ---- */

void cache_flush_page(struct ssd *ssd, lpn_t lpn)
{
    Cxlssd *ctx = cxlssd_ctx_from_ssd(ssd);
    Cache *c;
    CacheEntry key = { .lpn = lpn };
    CacheEntry *e;

    if (!ctx || !ctx->cache || !ctx->cache->cache_data) {
        return;
    }
    c = (Cache *)ctx->cache->cache_data;
    if (!c->cache_buf || !c->nand_buf) {
        return;
    }
    /* guard: a flush to an out-of-range (or window-tail control page) lpn
     * means a corrupted/ghost CacheEntry — writing slot content there would
     * silently destroy the mailbox/results area. Refuse loudly. */
    if ((uint64_t)lpn >= (uint64_t)(c->nand_size >> 12) ||
        (ctx->der_kvm && ctx->der_kvm->init_done && !ctx->der_kvm->plain &&
         lpn + DER_TAIL_PIN_PAGES >= (ctx->der_kvm->memory_size >> 12))) {
        fprintf(stderr, "Cylon cache: !! flush REFUSED bad/tail lpn %lld "
                "(nand pages %lld)\n", (long long)lpn,
                (long long)(c->nand_size >> 12));
        return;
    }
    e = g_tree_lookup(c->tree, &key);
    if (!e) {
        return;
    }
    memcpy((char *)c->nand_buf + (size_t)(lpn * CACHE_PAGE_SIZE),
           (const char *)c->cache_buf + (size_t)(e->slot_id * CACHE_PAGE_SIZE),
           CACHE_PAGE_SIZE);
}

void cache_set_backend(Cache *c, void *cache_buf, int64_t cache_buf_size,
                       void *nand_buf, int64_t nand_size)
{
    if (!c) return;
    c->cache_buf = cache_buf;
    c->cache_buf_size = cache_buf_size;
    c->nand_buf = nand_buf;
    c->nand_size = nand_size;
    c->nr_slots = (cache_buf_size > 0 && cache_buf) ? (uint32_t)(cache_buf_size / CACHE_PAGE_SIZE) : 0;
    g_free(c->free_slots);
    c->free_slots = c->nr_slots ? g_malloc(sizeof(uint32_t) * c->nr_slots) : NULL;
    for (uint32_t i = 0; i < c->nr_slots; i++) {
        c->free_slots[i] = i;
    }
    c->free_top = c->nr_slots;
}

Cache *cache_create(struct ssd *ssd, int policy_id, int size, CacheWay way)
{
    const CachePolicy *policy = cache_policy_get(policy_id);
    if (!policy || size <= 0) {
        return NULL;
    }

    Cache *c = g_malloc0(sizeof(Cache));
    c->ssd = ssd;
    c->policy_id = policy_id;
    c->policy = policy;
    c->size = size;
    c->way = way;
    c->nr_sets = (way == CACHE_WAY_FULL) ? 1 : (size + (1 << way) - 1) / (1 << way);
    if (c->nr_sets <= 0) {
        c->nr_sets = 1;
    }
    c->tree = g_tree_new(lpn_cmp);
    c->policy_private = NULL;
    c->cache_buf = NULL;
    c->nand_buf = NULL;
    c->nr_slots = 0;
    c->free_slots = NULL;
    c->free_top = 0;
    qemu_mutex_init(&c->lock);
    c->sets = g_malloc0(sizeof(CacheSet) * (size_t)c->nr_sets);
    for (int i = 0; i < c->nr_sets; i++) {
        QTAILQ_INIT(&c->sets[i].queue);
        c->sets[i].policy_private = NULL;
    }

    if (policy->init) {
        policy->init(c);
    }
    return c;
}

void cache_destroy(Cache *c)
{
    if (!c) return;
    if (c->policy && c->policy->cleanup) {
        c->policy->cleanup(c);
    }
    if (c->sets) {
        g_free(c->sets);
    }
    if (c->tree) {
        g_tree_destroy(c->tree);
    }
    g_free(c->free_slots);
    qemu_mutex_destroy(&c->lock);
    g_free(c);
}

CacheEntry *cache_lookup(Cache *c, lpn_t lpn)
{
    CacheEntry key = { .lpn = lpn };
    CacheEntry *e;

    /* The PNM engine thread and the FTL thread share this tree. The
     * formerly unlocked lookup raced insert's evict+free: a hit-path
     * entry could be freed mid-use and re-inserted with heap garbage
     * as lpn (non-canonical fill address -> host #GP, the poll_2 GPF).
     * Every tree access must hold c->lock. */
    qemu_mutex_lock(&c->lock);
    e = g_tree_lookup(c->tree, &key);
    qemu_mutex_unlock(&c->lock);
    return e;
}

uint32_t cylon_cache_lookup_slot(Cache *c, lpn_t lpn)
{
    CacheEntry key = { .lpn = lpn };
    CacheEntry *e;
    uint32_t slot;

    qemu_mutex_lock(&c->lock);
    e = g_tree_lookup(c->tree, &key);
    slot = e ? e->slot_id : UINT32_MAX;
    qemu_mutex_unlock(&c->lock);
    return slot;
}

/* High-level insert: eviction (epte_set_trap + flush), memcpy, epte_set_direct, then policy insert */
int cylon_cache_insert(Cache *c, CacheEntry *entry, int prefetch, bool guest)
{
    struct ssd *ssd = cache_get_ssd(c);
    Cxlssd *ctx = cxlssd_ctx_from_ssd(ssd);
    CacheSet *set = cache_get_set(c, entry->lpn);
    CacheWay way = cache_get_way(c);
    int ent_max = (way == CACHE_WAY_FULL) ? cache_get_size(c) : (1 << way);
    CacheEntry key = { .lpn = entry->lpn };
    int rc;
    bool evicted = false;

    (void)prefetch;

    if (!c->policy || !c->policy->insert_entry || !c->policy->evict_victim) {
        return -1;
    }

    /* out-of-range lpn: a garbage offset/neighbor id must never allocate a
     * slot (the fill would read past nand_buf) — the one-sided tail check
     * below happened to catch these too, but with its own message */
    if (c->nand_size && (uint64_t)entry->lpn >= (uint64_t)(c->nand_size >> 12)) {
        static unsigned warned;
        if (warned++ < 8) {
            fprintf(stderr, "Cylon cache: !! insert REFUSED out-of-range lpn %lld "
                    "(nand pages %lld)\n", (long long)entry->lpn,
                    (long long)(c->nand_size >> 12));
        }
        return -1;
    }
    /* tail control pages (mailbox/results/query) never enter the cache:
     * they self-pin direct at their first trap (cxlssd.c) */
    if (ctx && ctx->der_kvm && ctx->der_kvm->init_done && !ctx->der_kvm->plain &&
        entry->lpn + DER_TAIL_PIN_PAGES >= (ctx->der_kvm->memory_size >> 12)) {
        static unsigned warned;
        if (warned++ < 8) {
            fprintf(stderr, "Cylon cache: !! insert REFUSED tail lpn %lld\n",
                    (long long)entry->lpn);
        }
        return -1;
    }

    qemu_mutex_lock(&c->lock);

    if (g_tree_lookup(c->tree, &key)) {
        qemu_mutex_unlock(&c->lock);
        return c->policy->insert_entry(c, entry);
    }

    while (set->count >= ent_max) {
        CacheEntry *victim = c->policy->evict_victim(c, set);
        if (!victim) {
            qemu_mutex_unlock(&c->lock);
            return -1;
        }
        if (ctx && ctx->der_kvm) {
            if (der_kvm_epte_set_trap(ctx, victim->lpn) < 0) {
                fprintf(stderr, "Cylon cache: failed to set trap EPTE for page %lld\n",
                        (long long)victim->lpn);
            }
        }
        if (femu_cxldbg_on()) {
            fprintf(stderr, "Cylon cache: evicted page %lld from slot %u (hpa 0x%llx)\n",
                    (long long)victim->lpn, victim->slot_id,
                    (long long)ctx->cache_backend.hpa_base + (uint64_t)victim->slot_id * CACHE_PAGE_SIZE);
        }

        cache_flush_page(ssd, victim->lpn);
        g_tree_remove(c->tree, victim);
        cache_dec_entry_count(c);
        cache_dec_set_count(set);
        c->stats.evict_count++;
        entry->slot_id = victim->slot_id;
        cache_entry_free(victim);
        evicted = true;
    }

    /* Every eviction broke a live direct translation (leaf rewritten via the
     * userspace alias, invisible to KVM). Flush the vCPU EPT TLBs BEFORE the
     * freed slot is refilled below, so a stale read of the victim page traps
     * into the (correct) FTL path instead of silently returning the new
     * occupant's data. Guest-origin misses must flush (the vCPU reads the
     * window through these EPTEs); engine-origin misses can skip the flush —
     * host-side cache reads bypass EPT, so the DSE miss model (misses x
     * pg_rd_lat) stays free of this host-only IPI artifact (FEMU_DER_FLUSH=2
     * forces always-on, e.g. for collaborative CPU+engine runs). */
    if (evicted) {
        der_kvm_flush_tlbs(ctx, guest);
    }

    if (entry->slot_id == UINT32_MAX && c->cache_buf && c->nr_slots > 0) {
        if (c->free_top > 0) {
            entry->slot_id = c->free_slots[--c->free_top];
        } else {
            /* no free slot and the set didn't need an eviction (multi-set
             * ways): refuse rather than alias a live entry's slot */
            qemu_mutex_unlock(&c->lock);
            return -1;
        }
    }

    if (c->cache_buf && c->nand_buf && c->nr_slots > 0) {
        /* The entry bounds check ran before the lock; a residual race could
         * scribble the entry between the two. Refuse cleanly (slot returned
         * to the free stack; the guest re-traps and refills) instead of
         * faulting the host on a non-canonical fill address. */
        if ((uint64_t)entry->lpn >= (uint64_t)(c->nand_size >> 12)) {
            fprintf(stderr, "!! Cylon cache: insert REFUSED under-lock lpn 0x%llx "
                    "(entry %p, slot %u)\n", (unsigned long long)entry->lpn,
                    (void *)entry, entry->slot_id);
            if (entry->slot_id != UINT32_MAX) {
                c->free_slots[c->free_top++] = entry->slot_id;
            }
            qemu_mutex_unlock(&c->lock);
            return -1;
        }
        memcpy((char *)c->cache_buf + (size_t)(entry->slot_id * CACHE_PAGE_SIZE),
               (const char *)c->nand_buf + (size_t)(entry->lpn * CACHE_PAGE_SIZE),
               CACHE_PAGE_SIZE);
    }

    if (ctx && ctx->der_kvm && ctx->cache_backend.buf_space) {
        uint64_t hpa = ctx->cache_backend.hpa_base
            + (uint64_t)entry->slot_id * CACHE_PAGE_SIZE;
        if (der_kvm_epte_set_driect(ctx, entry->lpn, hpa) < 0) {
            fprintf(stderr, "Cylon cache: failed to set direct EPTE for page %lld (hpa 0x%llx)\n",
                    (long long)entry->lpn, (long long)hpa);
        }
    }

    if (femu_cxldbg_on()) {
        fprintf(stderr, "Cylon cache: inserted page %lld into slot %u (hpa 0x%llx)\n",
                (long long)entry->lpn, entry->slot_id,
                (long long)ctx->cache_backend.hpa_base + (uint64_t)entry->slot_id * CACHE_PAGE_SIZE);
    }
    rc = c->policy->insert_entry(c, entry);
    qemu_mutex_unlock(&c->lock);
    return rc;
}

/* Experiment-level cold reset: write every resident slot back to NAND,
 * drop all entries (through each policy's own evict path so per-policy
 * state stays consistent), re-trap every window page and restart the slot
 * allocator. Called from the PNM FLUSH job between experiments so a re-run
 * on the same FEMU starts from a cold, coherent cache — leftover direct
 * EPTEs from the previous run would otherwise let guest staging bypass the
 * FTL entirely (observed: 3s staging + BIND bad magic). */
void cylon_cache_reset(Cache *c)
{
    struct ssd *ssd = cache_get_ssd(c);
    Cxlssd *ctx = cxlssd_ctx_from_ssd(ssd);

    if (!c->policy || !c->policy->evict_victim) {
        return;
    }

    qemu_mutex_lock(&c->lock);
    for (int s = 0; s < c->nr_sets; s++) {
        CacheSet *set = &c->sets[s];
        while (set->count > 0) {
            CacheEntry *victim = c->policy->evict_victim(c, set);
            if (!victim) {
                break;      /* policy bookkeeping off; stop, don't spin */
            }
            if (ctx && ctx->der_kvm) {
                der_kvm_epte_set_trap(ctx, victim->lpn);
            }
            cache_flush_page(ssd, victim->lpn);
            g_tree_remove(c->tree, victim);
            cache_dec_entry_count(c);
            cache_dec_set_count(set);
            c->stats.evict_count++;
            cache_entry_free(victim);
        }
    }
    /* every entry (and its slot) was dropped above: rebuild the free stack */
    for (uint32_t i = 0; i < c->nr_slots; i++) {
        c->free_slots[i] = i;
    }
    c->free_top = c->nr_slots;
    qemu_mutex_unlock(&c->lock);

    /* all window leaves were just re-trapped: flush any stale direct
     * translations so post-FLUSH guest reads walk into the FTL path */
    der_kvm_flush_tlbs(ctx, true);
}

/* ---- Plugin ops (cache_ops_t: void *cache_data, struct cache_entry *) ---- */

static struct cache_entry *plugin_lookup(void *cache_data, lpn_t lpn)
{
    Cache *c = (Cache *)cache_data;
    return (struct cache_entry *)cache_lookup(c, lpn);
}

static void plugin_set_dirty(struct cache_entry *e, bool dirty)
{
    if (e) {
        ((CacheEntry *)e)->dirty = dirty;
    }
}

static bool plugin_is_dirty(struct cache_entry *e)
{
    return e ? ((CacheEntry *)e)->dirty : false;
}

static void plugin_insert(void *cache_data, struct cache_entry *e, int prefetch,
                          bool guest)
{
    Cache *c = (Cache *)cache_data;
    cylon_cache_insert(c, (CacheEntry *)e, prefetch, guest);
}

static struct cache_entry *plugin_entry_init(void *cache_data, lpn_t lpn)
{
    CacheEntry *e = g_malloc0(sizeof(CacheEntry));
    e->lpn = lpn;
    e->dirty = false;
    e->policy_data = NULL;
    e->slot_id = UINT32_MAX;    /* "unassigned" — 0 is a real slot */
    (void)cache_data;
    return (struct cache_entry *)e;
}

static uint32_t plugin_get_slot_id(struct cache_entry *e)
{
    return e ? ((CacheEntry *)e)->slot_id : 0;
}

static const cache_ops_t cylon_cache_ops = {
    .lookup = plugin_lookup,
    .set_dirty = plugin_set_dirty,
    .is_dirty = plugin_is_dirty,
    .insert = plugin_insert,
    .entry_init = plugin_entry_init,
    .get_slot_id = plugin_get_slot_id,
};

/* ---- Plugin create/destroy ---- */

static CacheWay cache_way_from_rep(uint8_t buffer_way)
{
    return (buffer_way >= CACHE_WAY_FULL) ? CACHE_WAY_FULL : (CacheWay)buffer_way;
}

static bool policy_registered;

struct cache_plugin *cylon_cache_plugin_create(FemuCtrl *n)
{
    if (!n->ssd || n->bufsz == 0) {
        return NULL;
    }
    if (!policy_registered) {
        cache_policy_register_all();
        policy_registered = true;
    }

    int size = (int)((n->bufsz * 1024ULL * 1024ULL) / 4096);
    if (size <= 0) {
        size = 1024;
    }
    CacheWay way = cache_way_from_rep(n->buffer_way);
    int policy_id = (int)n->rep;
    if (policy_id <= CACHE_POLICY_NONE || policy_id >= CACHE_POLICY_MAX) {
        policy_id = CACHE_POLICY_LIFO;
    }

    Cache *c = cache_create(n->ssd, policy_id, size, way);
    if (!c) {
        return NULL;
    }
    {
        Cxlssd *ctx = cxlssd_ctx_from_ctrl(n);
        if (ctx && ctx->cache_backend.buf_space && ctx->cache_backend.buf_size > 0 && n->mbe) {
            cache_set_backend(c, ctx->cache_backend.buf_space, ctx->cache_backend.buf_size,
                             n->mbe->logical_space, n->mbe->size);
        }
    }

    struct cache_plugin *p = g_malloc0(sizeof(struct cache_plugin));
    p->cache_data = c;
    p->ops = cylon_cache_ops;
    return p;
}

void cylon_cache_plugin_destroy(struct cache_plugin *p)
{
    if (!p) return;
    cache_destroy((Cache *)p->cache_data);
    g_free(p);
}
