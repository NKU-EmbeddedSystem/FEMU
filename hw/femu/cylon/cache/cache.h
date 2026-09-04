#ifndef __CYLON_CACHE_H
#define __CYLON_CACHE_H

#include "qemu/osdep.h"
#include "qemu/queue.h"
#include "qemu/thread.h"
#include <glib.h>

struct ssd;

/* Cache way: 1 = single entry per set; FULL = use full capacity per set */
typedef enum CacheWay {
    CACHE_WAY_1 = 0,
    CACHE_WAY_2,
    CACHE_WAY_4,
    CACHE_WAY_8,
    CACHE_WAY_16,
    CACHE_WAY_FULL
} CacheWay;

typedef uint64_t lpn_t;

#define CACHE_PAGE_SIZE  4096

typedef struct CacheEntry {
    lpn_t lpn;
    bool dirty;
    void *policy_data;
    uint32_t slot_id;   /* index into cache_backend (prealloc) for this page */
    QTAILQ_ENTRY(CacheEntry) entry;
} CacheEntry;

typedef struct CacheSet {
    CacheEntry *entry;       /* single entry when way == CACHE_WAY_1 */
    QTAILQ_HEAD(, CacheEntry) queue;
    int count;
    void *policy_private;    /* per-set policy data (e.g. S3FIFO: ghost/small lists; CLOCK: hand) */
} CacheSet;

typedef struct CacheStats {
    uint64_t insert_count;
    uint64_t evict_count;
} CacheStats;

struct CachePolicy;

typedef struct Cache {
    struct ssd *ssd;
    int policy_id;
    const struct CachePolicy *policy;
    int size;                /* max entries */
    CacheWay way;
    GTree *tree;             /* lpn -> CacheEntry */
    void *policy_private;    /* per-policy data (e.g. S3FIFO: ghost_tree) */
    CacheSet *sets;
    int nr_sets;
    int entry_count;
    CacheStats stats;
    void *backend;           /* for direct MR / EPT (optional) */
    /* Preallocated cache_backend (DER) and NAND backend for memcpy */
    void    *cache_buf;      /* cache_backend.buf_space */
    int64_t cache_buf_size;
    void    *nand_buf;       /* mbe->logical_space */
    int64_t nand_size;
    /* Free-slot LIFO stack: slot ownership is exclusive by construction —
     * a slot enters service only by being popped here (or by direct transfer
     * from an evicted victim inside cylon_cache_insert) and returns only in
     * cylon_cache_reset. The previous sentinel-0 + next_slot%nr_slots
     * allocator could hand a live entry's slot to a second entry (aliasing:
     * one page's fill overwrote the other's data). */
    uint32_t *free_slots;
    uint32_t free_top;
    uint32_t nr_slots;
    /* Guards tree/policy mutations (FTL thread) against concurrent lookups
     * from the PNM engine thread; GTree is not thread-safe. */
    QemuMutex lock;
} Cache;

/* Flush one page from cache to NAND (implemented in FTL) */
void cache_flush_page(struct ssd *ssd, lpn_t lpn);

Cache *cache_create(struct ssd *ssd, int policy_id, int size, CacheWay way);
void cache_destroy(Cache *c);

CacheEntry *cache_lookup(Cache *c, lpn_t lpn);
int cylon_cache_insert(Cache *c, CacheEntry *entry, int prefetch);

/* Cold reset for experiment re-runs on a live FEMU: flush every resident
 * slot back to NAND, drop all entries via the policy's own evict path,
 * re-trap every window EPTE and restart the slot allocator. */
void cylon_cache_reset(Cache *c);

/* Thread-safe slot lookup for cross-thread readers (e.g. the PNM engine):
 * returns the slot id by value, or UINT32_MAX on miss. Unlike
 * cache_lookup() the returned value stays valid after the lock is dropped
 * (no dangling CacheEntry pointer if the FTL thread evicts the page). */
uint32_t cylon_cache_lookup_slot(Cache *c, lpn_t lpn);

/* Set backends for NAND <-> cache_backend memcpy (call after create) */
void cache_set_backend(Cache *c, void *cache_buf, int64_t cache_buf_size,
                       void *nand_buf, int64_t nand_size);

#endif
