#ifndef __FEMU_KVM_EXT_H_
#define __FEMU_KVM_EXT_H_

#include "hw/femu/femu.h"
#include "cxlssd.h"

#include <linux/kvm.h>
#include "sysemu/kvm.h"

#define KVM_MEMSLOT_DUAL_MODE	(1UL << 17)
#define PAGE_SHIFT 12
#define MAX_ORDER 10
#define MAX_CONT_ALLOC_SZ (1<< (MAX_ORDER + PAGE_SHIFT))

typedef unsigned long long u64;
typedef uint64_t lpn_t;

struct kvm_set_epte_flag {
    uint64_t gpa;
    uint64_t flag;
    lpn_t lpn;
};

/* LEAF EPT entries */
struct kvm_memslot_linear_ept {
    u64* ept;
    int npages;
    int offset;
};

struct kvm_memslot_get_linear_ept {
    struct kvm_memslot_linear_ept ept_list[60];
    void *backend_ptr;
    u64 gfn;
    int n;
};

#define DER_KVM_SLOT_ID (0x2AU)

/* window-tail pages pinned EPT-direct to logical_space: covers the PNM
 * mailbox / results / query scratch (PNM_*_OFF_FROM_END <= 77824, so 32
 * pages = 128KB with margin). Never cache-resident, never flipped. */
#define DER_TAIL_PIN_PAGES  32

/* Per-CXLSSD KVM memslot/EPT state (lives in Cxlssd.der_kvm) */
typedef struct DerKvmState {
    struct kvm_memslot_get_linear_ept ept;
    bool init_done;
    bool plain;              /* plain RAM memslot, no DUAL_MODE/linear EPT */
    bool pinning;            /* inside the init tail-pin loop (tripwire off) */
    void *userspace_addr;
    uint64_t memory_size;
    uint64_t guest_phys_addr;
    uint64_t hpa_base;
} DerKvmState;

#define KVM_SET_EPTE_FLAG		  _IOW(KVMIO, 0xdd, struct kvm_set_epte_flag)
#define KVM_GET_LINEAR_EPT		  _IOWR(KVMIO, 0xde, struct kvm_memslot_get_linear_ept)
#define KVM_DER_FLUSH_TLB		  _IO(KVMIO, 0xdf)

int der_kvm_epte_set_trap(Cxlssd *ctx, uint64_t lpn);
int der_kvm_epte_set_driect(Cxlssd *ctx, uint64_t lpn, uint64_t hpa);
/* Invalidate vCPU EPT TLBs after leaf rewrites (evictions). Mode via
 * FEMU_DER_FLUSH: 0 = off, 1 (default) = flush on guest-origin misses only,
 * 2 = always. Kernels without the KVM_DER_FLUSH_TLB ioctl log once and
 * behave like the old flush-less code. */
int der_kvm_flush_tlbs(Cxlssd *ctx, bool guest);
/* Pin a window-tail control page direct to logical_space (call from the
 * trap path — see der_kvm.c). Never cachable, never flipped again. */
void der_kvm_pin_tail_page(Cxlssd *ctx, uint64_t lpn);
/* D1 Type-2 BI bill: re-trap a tail control page (mailbox/results) at job
 * completion so the client's next pickup read takes an EPT violation; the
 * trap path charges the snoop-equivalent latency (cylon_bi_lat_ns). Needs
 * a vCPU EPT TLB flush or clients with a cached direct translation never
 * see the trap (silently no-op'd in plain mode / before init). */
void der_kvm_epte_retrap_control(Cxlssd *ctx, uint64_t lpn);
/* BI snoop-equivalent latency (ns), 0 = off. Defined in der_kvm.c, written
 * by the PNM thread (live knob), charged in cxlssd.c's tail-page trap path. */
extern uint64_t cylon_bi_lat_ns;
uint64_t *der_kvm_get_eptep_dbg(Cxlssd *ctx, uint64_t lpn);
/* D3-F F5: KVM_EXIT_CYLON_DER "bill & re-execute" handler (called from
 * accel/kvm/kvm-all.c on a DUAL-slot EPT violation with no instruction
 * decode). Bills via the same paths as the legacy trap path (tail: pin +
 * BI charge; data: FTL bill + cache insert + EPTE flip) and leaves the
 * leaf direct so the guest instruction re-executes natively -- fixes the
 * avx #UD family (KVM emulator cannot decode VEX) with real flips intact.
 * Returns <0 on refusal (plain/skip_ftl mode); KVM then falls back to the
 * legacy emulator path via its per-gpa retry cap. */
int cylon_der_handle_fault(uint64_t gpa, uint8_t is_write);

int der_kvm_set_user_memory_region(const FemuCtrl *n);
/* Acceptance/debug mode: register the window as a plain RAM memslot (EPT
 * direct, zero exits). Guest stores go straight into logical_space. */
int der_kvm_set_user_memory_region_plain(const FemuCtrl *n);
int der_kvm_del_user_memory_region(const FemuCtrl *n);

#endif