// cylon_index.cpp -- faiss::Index adapter over libcylon (C ABI).
#include "cylon_index.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace faiss_cylon {

CylonIndex::CylonIndex(int d, const cylon_config *cfg_in)
    : faiss::Index(d, faiss::METRIC_L2)
{
    if (!cfg_in) {
        throw faiss::FaissException("CylonIndex: cfg required");
    }
    cfg_ = *cfg_in;
    cylon_status rc = cylon_open(&ctx_, &cfg_);
    if (rc != CYLON_ST_OK) {
        throw faiss::FaissException(std::string("CylonIndex: cylon_open: ") +
                                    cylon_strerror(rc));
    }
    rc = cylon_load(ctx_);
    if (rc != CYLON_ST_OK) {
        cylon_close(ctx_);
        ctx_ = nullptr;
        throw faiss::FaissException(std::string("CylonIndex: cylon_load: ") +
                                    cylon_strerror(rc));
    }
    if (cylon_dim(ctx_) != (uint32_t)d) {
        uint32_t bd = cylon_dim(ctx_);
        cylon_close(ctx_);
        ctx_ = nullptr;
        throw faiss::FaissException(std::string("CylonIndex: blob dim ") +
                                    std::to_string(bd) + " != ctor d " +
                                    std::to_string(d));
    }
}

void CylonIndex::add(faiss::idx_t, const float *)
{
    throw faiss::FaissException(
        "CylonIndex: CYH1 is a pre-built index; add() is not supported "
        "(use the host builder toolchain to make a blob)");
}

void CylonIndex::reset()
{
    throw faiss::FaissException("CylonIndex: reset() not supported");
}

void CylonIndex::search(faiss::idx_t n, const float *x, faiss::idx_t k,
                        float *distances, faiss::idx_t *labels,
                        const faiss::SearchParameters *) const
{
    if (!ctx_) {
        throw faiss::FaissException("CylonIndex: context closed");
    }
    if (k < 1 || k > CYLON_KMAX) {
        throw faiss::FaissException(std::string("CylonIndex: k out of range "
                                                "1..") +
                                    std::to_string(CYLON_KMAX));
    }
    std::vector<uint32_t> ids32((size_t)n * k);
    cylon_status rc = cylon_search(ctx_, CYLON_PREC_F32, x, (uint32_t)n,
                                   (uint32_t)k, 0, ids32.data(), distances,
                                   &stats_);
    if (rc != CYLON_ST_OK) {
        throw faiss::FaissException(std::string("CylonIndex: search: ") +
                                    cylon_strerror(rc));
    }
    for (size_t i = 0; i < (size_t)n * k; i++) {
        labels[i] = (faiss::idx_t)ids32[i];    /* u32 label space zero-extends;  */
    }                                    /* padding 0xffffffff passes thru */
}

CylonIndex::~CylonIndex()
{
    if (ctx_) {
        cylon_close(ctx_);
        ctx_ = nullptr;
    }
}

} // namespace faiss_cylon
