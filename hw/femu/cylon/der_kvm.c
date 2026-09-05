#include "der_kvm.h"
#include "cache/cache_backend.h"
#include "hw/core/cpu.h"
#include "sysemu/kvm.h"
#include "hw/femu/femu.h"     /* femu_cxldbg_on() */
/* kvm.h declares kvm_vm_ioctl only inside #ifdef NEED_CPU_H; provide it when building without */
#ifndef NEED_CPU_H
extern int kvm_vm_ioctl(KVMState *s, int type, ...);
#endif
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#define DIRECT_MASK  0x600000000000977ULL
#define MMIO_MASK    0x0000000586ULL

/* Real HPA of a page in our own address space via /proc/self/pagemap
 * (FEMU runs as root; logical_space is anonymous, pre-faulted and mlocked
 * by init_dram_backend, so the translation is stable). */
static uint64_t der_kvm_pagemap_hpa(const void *page)
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

/* EPT chunk index for an lpn. Chunks are MAX_CONT_ALLOC_SZ (4MB) = 524288
 * entries, so chunk = lpn >> 19. (A previous (lpn*8)>>20 = lpn>>17 was 4x
 * too large: every page above 512MB of window indexed past its chunk and
 * get_eptep() returned NULL, so mailbox/result pages at the 48G tail could
 * never be flipped to direct EPTEs.) */
static inline u64 get_leaf_ept_idx(u64 lpn)
{
    return lpn >> (MAX_ORDER + PAGE_SHIFT - 3);
}

/* Allocate and fetch linear EPT (full CXL_SSD space). */
static int init_leaf_ept(DerKvmState *s)
{
    KVMState *kvm = kvm_state;
    u64 size = (s->memory_size >> PAGE_SHIFT) * sizeof(u64 *);
    int idx = 0;
    int ret;

    while (size > 0) {
        u64 sz = (size > MAX_CONT_ALLOC_SZ) ? MAX_CONT_ALLOC_SZ : size;
        s->ept.ept_list[idx].ept = mmap(NULL, (size_t)sz, PROT_READ | PROT_WRITE,
                                       MAP_ANONYMOUS | MAP_SHARED, -1, 0);
        if (s->ept.ept_list[idx].ept == MAP_FAILED) {
            perror("der_kvm: mmap ept failed");
            abort();
        }
        /* first lpn covered by this chunk, in units of 512 lpns
         * (4MB chunk / 8B per entry = 524288 lpns = 1024 * 512) */
        s->ept.ept_list[idx].offset = (u64)idx * 1024;
        size -= sz;
        idx++;
    }

    s->ept.gfn = s->guest_phys_addr >> PAGE_SHIFT;
    s->ept.backend_ptr = s->userspace_addr;

    ret = kvm_vm_ioctl(kvm, KVM_GET_LINEAR_EPT, &s->ept);
    if (ret < 0) {
        perror("der_kvm: KVM_GET_LINEAR_EPT");
        abort();
    }
    fprintf(stderr, "Cylon DER-KVM: init_leaf_ept done (gfn 0x%" PRIx64 ", size %" PRIu64 " pages)\n",
            (uint64_t)s->ept.gfn, (uint64_t)(s->memory_size >> PAGE_SHIFT));
    return 0;
}

static u64 *get_eptep(DerKvmState *s, u64 lpn)
{
    if (!s || !s->init_done) {
        return NULL;
    }
    u64 idx = get_leaf_ept_idx(lpn);
    if (idx >= 60 || !s->ept.ept_list[idx].ept) {
        return NULL;
    }
    u64 off = lpn - (u64)s->ept.ept_list[idx].offset * 512;
    if (off >= 524288) {
        return NULL;
    }
    return s->ept.ept_list[idx].ept + off;
}

/* debug: expose leaf-entry pointer for live dumping (NULL in plain mode) */
uint64_t *der_kvm_get_eptep_dbg(Cxlssd *ctx, uint64_t lpn)
{
    DerKvmState *s = ctx ? ctx->der_kvm : NULL;
    if (!s || !s->init_done || s->plain) {
        return NULL;
    }
    return (uint64_t *)get_eptep(s, lpn);
}

/* Flip one window-tail page's EPTE direct to its logical_space HPA. Must be
 * called from the TRAP path: KVM's dual-mode leaves materialize on first
 * walk (populated with the default trap value), so a leaf pre-written at
 * init gets overwritten on first walk — observed when the init-time pin
 * failed and the mailbox page landed in cache slot 0 at the first job.
 * Called with the leaf materialized, the write sticks like any FTL flip.
 * Idempotent. */
void der_kvm_pin_tail_page(Cxlssd *ctx, uint64_t lpn)
{
    DerKvmState *s = ctx ? ctx->der_kvm : NULL;
    uint64_t hpa;

    if (!s || !s->init_done || s->plain) {
        return;
    }
    if (lpn + DER_TAIL_PIN_PAGES < (s->memory_size >> 12)) {
        return;     /* not a tail page */
    }
    hpa = der_kvm_pagemap_hpa((const char *)s->userspace_addr + (lpn << 12));
    if (!hpa) {
        fprintf(stderr, "Cylon DER-KVM: !! tail pin: no HPA for lpn %llu\n",
                (long long)lpn);
        return;
    }
    s->pinning = true;      /* legit tail flip: tripwire off */
    der_kvm_epte_set_driect(ctx, lpn, hpa);
    s->pinning = false;
}

/* BI snoop-equivalent latency (ns) charged in the tail-page trap path
 * (cxlssd.c). Written by the PNM thread from the live knob per job; 0 =
 * off = E1'-identical behavior. */
uint64_t cylon_bi_lat_ns;

/* Re-trap a tail control page at job completion (D1 BI bill). pin_tail_page
 * made these pages direct forever; the BI semantic needs the client's next
 * pickup read to EXIT into FEMU so the snoop-equivalent latency can be
 * charged before flipping back. Tail flips are exactly what the set_trap
 * tripwire watches for, so write the leaf here directly. Must flush vCPU
 * EPT TLBs or a vCPU holding the cached direct translation never exits
 * and the bill is silently skipped (FEMU_DER_FLUSH=0 disables this too). */
void der_kvm_epte_retrap_control(Cxlssd *ctx, uint64_t lpn)
{
    DerKvmState *s = ctx ? ctx->der_kvm : NULL;
    uint64_t gfn;
    u64 *eptep;

    if (!s || !s->init_done || s->plain) {
        return;
    }
    if (lpn + DER_TAIL_PIN_PAGES < (s->memory_size >> 12)) {
        return;     /* not a tail page */
    }
    gfn = (s->guest_phys_addr >> PAGE_SHIFT) + lpn;
    eptep = get_eptep(s, lpn);
    if (eptep) {
        *eptep = (gfn << PAGE_SHIFT) | MMIO_MASK;
        der_kvm_flush_tlbs(ctx, true);
    }
}

int der_kvm_epte_set_trap(Cxlssd *ctx, uint64_t lpn)
{
    DerKvmState *s = ctx ? ctx->der_kvm : NULL;
    if (!s) {
        return -1;
    }
    if (s->plain) {
        return 0;   /* no per-page EPTE control in plain mode */
    }
    /* tripwire: the window tail (PNM mailbox/results/query) is pinned direct
     * at init and must NEVER be flipped again — a flip here means something
     * is trying to put a control page into a recyclable cache slot */
    if (s->init_done && lpn + DER_TAIL_PIN_PAGES >= (s->memory_size >> 12)) {
        fprintf(stderr, "Cylon DER-KVM: !! TAIL FLIP trap lpn=%llu caller=%p\n",
                (long long)lpn, __builtin_return_address(0));
    }
    uint64_t gfn = (s->guest_phys_addr >> PAGE_SHIFT) + lpn;
    u64 *eptep = get_eptep(s, lpn);
    if (eptep) {
        {
            static unsigned dbg_n;
            if (femu_cxldbg_on() && dbg_n < 64) {
                dbg_n++;
                fprintf(stderr, "CXLDBG TRAP lpn=%lld eptep=%p old=0x%" PRIx64 "\n",
                        (long long)lpn, (void *)eptep, (uint64_t)*eptep);
            }
        }
        *eptep = (gfn << PAGE_SHIFT) | MMIO_MASK;
        {
            static int verbose = -1;
            if (verbose < 0) {
                verbose = getenv("FEMU_DER_VERBOSE") ? 1 : 0;
            }
            if (verbose) {
                fprintf(stderr, "Cylon DER-KVM: set trap EPTE for page %lld (gfn 0x%llx)\n",
                        (long long)lpn, (long long)gfn);
            }
        }
        return 0;
    }
    return -1;
}

/* Invalidate all vCPU EPT TLB entries after leaf rewrites. The flip helpers
 * above write the shared leaves directly from userspace, so KVM never learns
 * the EPTE changed — a vCPU whose TLB still caches a page's old direct
 * translation keeps reading the OLD slot's content after eviction/reinsert
 * (host-side cache reads bypass EPT and are unaffected; guest window readers
 * like cpu_search hit this). Requires the CylonLinux KVM_DER_FLUSH_TLB ioctl
 * (0xdf); on kernels without it we warn once and keep the old behavior.
 * FEMU_DER_FLUSH=0 disables. */
int der_kvm_flush_tlbs(Cxlssd *ctx, bool guest)
{
    DerKvmState *s = ctx ? ctx->der_kvm : NULL;
    static int mode = -1;
    int ret;

    if (!s || s->plain) {
        return 0;
    }
    if (mode < 0) {
        const char *e = getenv("FEMU_DER_FLUSH");
        if (!e) {
            /* sudo scrubs env: run-cxlssd.sh publishes the mode via this
             * sentinel file instead (same pattern as /tmp/femu-der-disable) */
            FILE *f = fopen("/tmp/femu-der-flush", "r");
            if (f) {
                char buf[16] = { 0 };
                if (fgets(buf, sizeof(buf), f)) {
                    e = buf;
                }
                fclose(f);
            }
        }
        mode = e ? atoi(e) : 1;
        if (mode < 0 || mode > 2) {
            mode = 1;
        }
    }
    /* 0 = off; 1 = guest-origin misses only (engine reads bypass EPT, so
     * their evictions can skip the flush — keeps the engine miss model
     * free of this host-only IPI overhead); 2 = always */
    if (mode == 0 || (mode == 1 && !guest)) {
        return 0;
    }
    ret = kvm_vm_ioctl(kvm_state, KVM_DER_FLUSH_TLB, 0);
    if (ret < 0) {
        static bool warned;
        if (!warned) {
            warned = true;
            fprintf(stderr, "Cylon DER-KVM: KVM_DER_FLUSH_TLB unavailable "
                    "(ret=%d errno=%d) — stale EPT TLB after evictions WILL "
                    "corrupt guest window reads (host-side reads unaffected)\n",
                    ret, errno);
        }
        return -1;
    }
    return 0;
}

int der_kvm_epte_set_driect(Cxlssd *ctx, uint64_t lpn, uint64_t hpa)
{
    DerKvmState *s = ctx ? ctx->der_kvm : NULL;
    if (!s) {
        return -1;
    }
    if (s->plain) {
        return 0;   /* no per-page EPTE control in plain mode */
    }
    /* tripwire: see der_kvm_epte_set_trap; s->pinning lets the init loop
     * itself set the tail direct without tripping */
    if (s->init_done && !s->pinning &&
        lpn + DER_TAIL_PIN_PAGES >= (s->memory_size >> 12)) {
        fprintf(stderr, "Cylon DER-KVM: !! TAIL FLIP direct lpn=%llu hpa=0x%llx caller=%p\n",
                (long long)lpn, (long long)hpa, __builtin_return_address(0));
    }
    u64 *eptep = get_eptep(s, lpn);
    if (eptep) {
        {
            static unsigned dbg_n;
            if (femu_cxldbg_on() && dbg_n < 64) {
                dbg_n++;
                fprintf(stderr, "CXLDBG FLIP lpn=%lld eptep=%p old=0x%" PRIx64 " newhpa=0x%" PRIx64 "\n",
                        (long long)lpn, (void *)eptep, (uint64_t)*eptep, (uint64_t)hpa);
            }
        }
        *eptep = (hpa & ~(uint64_t)(4096 - 1)) | DIRECT_MASK;
        {
            static int verbose = -1;
            if (verbose < 0) {
                verbose = getenv("FEMU_DER_VERBOSE") ? 1 : 0;
            }
            if (verbose) {
                fprintf(stderr, "Cylon DER-KVM: set direct EPTE for page %lld (hpa 0x%llx)\n",
                        (long long)lpn, (long long)hpa);
            }
        }
        return 0;
    }
    return -1;
}

/*
 * Register the KVM memslot for the NAND region (full CXL SSD space).
 * DER then controls each page's EPTE: in-cache -> direct to cache backend HPA,
 * not in cache -> MMIO (trap to QEMU). init_leaf_ept() builds EPT for this same
 * range so we can set/clear direct/trap per page in O(1) time.
 */
int der_kvm_set_user_memory_region(const FemuCtrl *n)
{
    KVMState *kvm = kvm_state;
    struct kvm_userspace_memory_region mem;
    Cxlssd *ctx = cxlssd_ctx_from_ctrl((FemuCtrl *)n);
    DerKvmState *s;
    int ret;

    if (!n || !n->mbe) {
        return -1;
    }
    if (!ctx || !ctx->der_kvm) {
        return -1;
    }
    s = ctx->der_kvm;

    /* Memslot = NAND region (full CXL_SSD space); EPTEs updated per-page for cache vs trap */
    s->guest_phys_addr = n->base_gpa ? n->base_gpa : femu_get_base_gpa();
    if (!s->guest_phys_addr) {
        fprintf(stderr, "Cylon DER-KVM: guest_phys_addr is 0 (base_gpa not set)\n");
        return -1;
    }
    s->memory_size = n->mbe->size;
    s->userspace_addr = n->mbe->logical_space;
    s->hpa_base = 0; /* direct HPA is passed per-page in der_kvm_epte_set_driect() */

    mem.slot = DER_KVM_SLOT_ID;
    mem.guest_phys_addr = s->guest_phys_addr;
    mem.memory_size = s->memory_size;
    mem.userspace_addr = (uint64_t)(uintptr_t)s->userspace_addr;
    mem.flags = KVM_MEMSLOT_DUAL_MODE;

    fprintf(stderr, "Cylon DER-KVM: KVM_SET_USER_MEMORY_REGION: slot %u, gpa 0x%llx, size %lld, ua 0x%llx\n",
            mem.slot, mem.guest_phys_addr, mem.memory_size, mem.userspace_addr);    
    ret = kvm_vm_ioctl(kvm, KVM_SET_USER_MEMORY_REGION, &mem);
    if (ret < 0) {
        perror("Cylon DER-KVM: KVM_SET_USER_MEMORY_REGION");
        fprintf(stderr, "Cylon DER-KVM: KVM_SET_USER_MEMORY_REGION failed: %d\n", ret);
        return -1;
    }

    fprintf(stderr, "Cylon DER-KVM: NAND memslot registered (gpa 0x%" PRIx64 " size %" PRIu64 ")\n",
            (uint64_t)s->guest_phys_addr, (uint64_t)s->memory_size);
    init_leaf_ept(s);
    s->init_done = true;
    return 0;
}

/*
 * Acceptance/debug mode: register the window as a plain RAM memslot
 * (flags = 0, no DUAL_MODE, no linear EPT). Guest accesses become EPT
 * direct into logical_space: zero VM exits, no cfmws IO dispatch, no
 * FTL/cache involvement. The PNM engine reads the same logical_space, so
 * guest and engine stay coherent.
 */
int der_kvm_set_user_memory_region_plain(const FemuCtrl *n)
{
    KVMState *kvm = kvm_state;
    struct kvm_userspace_memory_region mem;
    Cxlssd *ctx = cxlssd_ctx_from_ctrl((FemuCtrl *)n);
    DerKvmState *s;
    int ret;

    if (!n || !n->mbe) {
        return -1;
    }
    if (!ctx || !ctx->der_kvm) {
        return -1;
    }
    s = ctx->der_kvm;

    s->guest_phys_addr = n->base_gpa ? n->base_gpa : femu_get_base_gpa();
    if (!s->guest_phys_addr) {
        fprintf(stderr, "Cylon DER-KVM: guest_phys_addr is 0 (base_gpa not set)\n");
        return -1;
    }
    s->memory_size = n->mbe->size;
    s->userspace_addr = n->mbe->logical_space;

    mem.slot = DER_KVM_SLOT_ID;
    mem.guest_phys_addr = s->guest_phys_addr;
    mem.memory_size = s->memory_size;
    mem.userspace_addr = (uint64_t)(uintptr_t)s->userspace_addr;
    mem.flags = 0;

    ret = kvm_vm_ioctl(kvm, KVM_SET_USER_MEMORY_REGION, &mem);
    if (ret < 0) {
        perror("Cylon DER-KVM: KVM_SET_USER_MEMORY_REGION (plain)");
        return -1;
    }

    s->plain = true;
    fprintf(stderr, "Cylon DER-KVM: plain RAM memslot registered (gpa 0x%" PRIx64
            " size %" PRIu64 ", EPT direct, zero-exit acceptance mode)\n",
            (uint64_t)s->guest_phys_addr, (uint64_t)s->memory_size);
    return 0;
}

/* Remove the NAND memslot and release the slot id. */
int der_kvm_del_user_memory_region(const FemuCtrl *n)
{
    KVMState *kvm = kvm_state;
    struct kvm_userspace_memory_region mem;
    Cxlssd *ctx = cxlssd_ctx_from_ctrl((FemuCtrl *)n);
    int ret = 0;

    if (!n || !n->mbe) {
        return -1;
    }
    if (ctx && ctx->der_kvm) {
        mem.slot = DER_KVM_SLOT_ID;
        mem.guest_phys_addr = n->base_gpa;
        mem.memory_size = 0;
        mem.userspace_addr = 0;
        mem.flags = 0;
        ret = kvm_vm_ioctl(kvm, KVM_SET_USER_MEMORY_REGION, &mem);
        ctx->der_kvm->init_done = false;
    }
    return ret;
}
