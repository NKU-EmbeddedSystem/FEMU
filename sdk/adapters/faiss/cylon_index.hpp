// cylon_index.hpp -- faiss::Index adapter over libcylon (C ABI).
//
// Story: CYH1 = the device-resident form of a pre-built ANNS graph index.
// A faiss user constructs CylonIndex(d, cfg) and gets a faiss::Index whose
// search() runs on the CXL-SSD accelerator; build-side (add/train/reset)
// stays on the host builder toolchain and is explicitly refused here.
//
// Precision: search() takes fp32 queries (faiss convention) and enters the
// C ABI at CYLON_PREC_F32; the library converts to the device storage
// precision (F16 for the v1 profile) with one RNE rounding per vector.
// Loss is disclosed via cylon_get_info (storage/accum/metric/in_prec_mask).
//
// Padding: result rows shorter than k carry id 0xffffffff / dist 0.0f and
// are passed through verbatim (doc decision 2: adapter does not remap).
#pragma once

#include <faiss/Index.h>
#include "cylon.h"

namespace faiss_cylon {

class CylonIndex : public faiss::Index {
public:
    /* d must match the CYH1 blob (re-checked after load); metric must be
     * L2 (v1 device profile). Opens the window, stages and binds the
     * blob -- construction = open + load. Throws faiss::FaissException
     * with a readable message on any device-side failure. */
    CylonIndex(int d, const cylon_config *cfg);

    ~CylonIndex() override;

    /* build-side: refused -- CYH1 is pre-built by the host builder. */
    void add(faiss::idx_t n, const float *x) override;
    void reset() override;

    void search(faiss::idx_t n, const float *x, faiss::idx_t k,
                float *distances, faiss::idx_t *labels,
                const faiss::SearchParameters *params = nullptr) const override;

    /* last search's engine-side stats, verbatim from the device resp
     * (engine-arm share only; see cylon.h cylon_stats). */
    cylon_stats stats() const { return stats_; }

    /* escape hatch for direct C ABI calls (cylon_get_info etc.) */
    cylon_ctx *handle() const { return ctx_; }

private:
    cylon_ctx *ctx_ = nullptr;
    cylon_config cfg_{};
    mutable cylon_stats stats_{};   /* written by const search() */
};

} // namespace faiss_cylon
