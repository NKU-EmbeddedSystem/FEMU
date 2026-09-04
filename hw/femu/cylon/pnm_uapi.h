/*
 * PNM (Processing-Near-Memory) user API — shared between FEMU (device side)
 * and the guest userspace client. Plain C, <stdint.h> only, no QEMU headers.
 *
 * Transport: a 4KB mailbox at the tail of the CXL window (not ivshmem!).
 * Both sides reach it via normal memory: guest through devdax mmap, FEMU
 * through cache-aware xlate (hit -> pmem cache slice, miss -> logical_space),
 * the same locations the guest's hardware path (EPT direct / trap) uses.
 * Soft-sync is therefore coherent by construction.
 *
 * Handshake: guest writes job + status=PENDING (release); FEMU engine polls,
 * runs op, writes resp + status=DONE (release); guest reads resp, sets IDLE.
 */
#ifndef __CYLON_PNM_UAPI_H
#define __CYLON_PNM_UAPI_H

#include <stdint.h>

#define PNM_UAPI_VERSION 1

/* opcodes */
#define PNM_OP_NOP          0
#define PNM_OP_BIND_INDEX   1   /* a0 = window offset of CYH1 index blob */
#define PNM_OP_ANNS_SEARCH  2   /* a0 = query_off, a1 = result_off, k, ef */
#define PNM_OP_CACHE_FLUSH  3   /* cold-start the device cache: write all
                                 * resident slots back to NAND, drop every
                                 * entry, re-trap every window EPTE. Issue
                                 * before staging so a re-run on a live FEMU
                                 * doesn't stage through stale direct EPTEs */

/* status codes */
#define PNM_ST_OK           0
#define PNM_ST_ENOSYS       1   /* unknown op */
#define PNM_ST_ENOINDEX     2   /* engine has no bound index */
#define PNM_ST_EINVAL       3
#define PNM_ST_ENOMEM       4

/* mailbox states */
#define PNM_MB_IDLE     0
#define PNM_MB_PENDING  1
#define PNM_MB_DONE     2

/* mailbox/scratch placement, offsets from END of the CXL window */
#define PNM_MB_OFF_FROM_END         4096
#define PNM_RESULTS_OFF_FROM_END    20480
#define PNM_QUERY_OFF_FROM_END      77824

/* job descriptor (64B, rounded by alignment) */
struct pnm_job_s {
    uint32_t job_id;
    uint32_t op;
    uint64_t a0;    /* BIND: index blob offset | SEARCH: query vector offset */
    uint64_t a1;    /* SEARCH: result area offset */
    uint32_t k;
    uint32_t ef;
    uint32_t status;
    uint32_t reserved0;
    uint64_t reserved1[2];
};

/* engine response (40B) */
struct pnm_resp_s {
    uint32_t job_id;
    uint32_t status;      /* PNM_ST_* */
    uint32_t n_found;     /* valid result entries written */
    uint32_t reserved;
    uint64_t total_ns;
    uint64_t n_dist;      /* distance computations */
    uint64_t n_hops;      /* neighbor-list expansions */
    uint64_t n_pages;     /* search-time cache misses (NAND page reads charged) */
};

/* mailbox (one outstanding job in Phase A) */
struct pnm_mb_s {
    struct pnm_job_s job;    /* 64B */
    struct pnm_resp_s resp;  /* 40B */
    uint32_t status;         /* IDLE/PENDING/DONE */
    uint32_t reserved;
};

/* CYH1 index blob (written by tools/export_hnsw.py, bound by engine).
 * All offsets relative to blob start. Vectors fp16, L2 distance.
 * Layout: header | vectors | level0 adj (fixed stride maxm0, 0xFFFFFFFF=end)
 *       | upper-level records (per-node: levels 1..lvl: u32 deg + ids)
 *       | levels array (count x u8)
 */
#define CYH1_MAGIC  0x31485943  /* "CYH1" */
#define CYH1_VERSION 1
struct cyh1_header {
    uint32_t magic, version, dim, count;
    uint32_t M, maxm0;
    uint32_t entry_point, entry_level;
    uint64_t off_vectors;
    uint64_t off_adj0;
    uint64_t off_upper;
    uint64_t off_levels;
};

#endif
