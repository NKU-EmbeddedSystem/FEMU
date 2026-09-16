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
#define PNM_OP_STAGE        4   /* device-initiated staging (D2, FTL-prefetch):
                                 * a0 = blob byte count; the engine bulk-fills
                                 * cache slots from its own media (mapped pages
                                 * in [0, a0)) at modeled bulk bandwidth
                                 * /tmp/femu-stage-bps (absent = 2 GB/s, 0 =
                                 * unbilled). a1 must be 0. Coverage lands in
                                 * resp.n_found; skipped pages in resp.n_pages.
                                 * Engine older than this op answers ENOSYS ->
                                 * client falls back to legacy first-touch. */

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

/* v2 state word: status and generation packed as one aligned u64 at the
 * tail of the mailbox, so each side publishes transitions with a single
 * atomic u64 store. Client owns gen (increments per job, kills ABA/replay
 * of stale PENDING); engine accepts PENDING only if gen == last+1.
 * gen==0 means a legacy v1 client (old binaries leave the high half 0) —
 * engine falls back to v1 semantics (no gen check). */
#define PNM_MB_PACK(gen, st)  ((((uint64_t)(gen)) << 32) | (uint32_t)(st))
#define PNM_MB_STATE(w)       ((uint32_t)((w) & 0xffffffffu))
#define PNM_MB_GEN(w)         ((uint32_t)((w) >> 32))

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

/* engine response (64B: legacy 40B prefix, PQ extras appended) */
struct pnm_resp_s {
    uint32_t job_id;
    uint32_t status;      /* PNM_ST_* */
    uint32_t n_found;     /* valid result entries written */
    uint32_t reserved;
    uint64_t total_ns;
    uint64_t n_dist;      /* distance computations */
    uint64_t n_hops;      /* neighbor-list expansions */
    uint64_t n_pages;     /* search-time cache misses (NAND page reads charged) */
    /* --- CYH2/PQ route extras (uapi v2; appended, legacy offsets kept) --- */
    uint64_t n_rerank;       /* full-precision rerank distances (0 = A0) */
    uint64_t n_code_pages;   /* code-region page misses (A1 codes region) */
    uint64_t n_vector_pages; /* vector-region page misses (rerank pulls) */
};

/* mailbox (one outstanding job; v2 state word packs status+gen as u64) */
struct pnm_mb_s {
    struct pnm_job_s job;    /* 64B */
    struct pnm_resp_s resp;  /* 40B */
    union {
        uint64_t state;      /* atomic view: (gen << 32) | status */
        struct {
            uint32_t status;     /* IDLE/PENDING/DONE (low half) */
            uint32_t gen;        /* job generation, client-owned (high) */
        };
    };
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

/* CYH2: AiSAQ-style PQ index container (branch cylon-v9.2-aisaq).
 * Header = the CYH1 prefix (legacy offsets remain valid per layout) + PQ
 * extension. A1 = CYH1 sections byte-identical + [codebook][codes] appended
 * after levels; A2 = vectors | upper | levels | codebook | node records
 * ([16B code | u32 deg | deg*u32 ids], offset table first, offsets relative
 * to section start; off_adj0 = 0).
 * rerank_R is the engine default; /tmp/femu-rerank-R overrides per job. */
#define CYH2_MAGIC   0x32485943  /* "CYH2" */
#define CYH2_VERSION 1
#define CYH2_LAYOUT_A1 1
#define CYH2_LAYOUT_A2 2
struct cyh2_header {
    struct cyh1_header cyh1;                 /* 64B legacy prefix */
    uint64_t off_codebook;
    uint64_t off_nodes;
    uint64_t off_codes;
    uint32_t pq_m, pq_nbits, rerank_R, layout_flags;
    uint64_t blob_bytes;
};
#endif
