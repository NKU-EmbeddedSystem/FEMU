// cylon_hnsw.cpp -- hnswlib adapter implementation (0.8.0 interface).
#include "cylon_hnsw.hpp"
#include <cstdio>
#include <cstring>
#include <queue>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hnsw_cylon {

CylonHnsw::CylonHnsw(const cylon_config *cfg_in)
    : cfg_{}
{
    if (!cfg_in) {
        throw std::runtime_error("CylonHnsw: cfg required");
    }
    cfg_ = *cfg_in;
    cylon_status rc = cylon_open(&ctx_, &cfg_);
    if (rc != CYLON_ST_OK) {
        throw std::runtime_error(std::string("CylonHnsw: cylon_open: ") +
                                 cylon_strerror(rc));
    }
    rc = cylon_load(ctx_);
    if (rc != CYLON_ST_OK) {
        cylon_close(ctx_);
        ctx_ = nullptr;
        throw std::runtime_error(std::string("CylonHnsw: cylon_load: ") +
                                 cylon_strerror(rc));
    }
}


CylonHnsw::~CylonHnsw()
{
    if (ctx_) {
        cylon_close(ctx_);
        ctx_ = nullptr;
    }
}

void CylonHnsw::addPoint(const void *, hnswlib::labeltype, bool)
{
    throw std::runtime_error(
        "CylonHnsw: CYH1 is a pre-built index; addPoint() is not supported "
        "(use the host builder toolchain to make a blob)");
}

void CylonHnsw::saveIndex(const std::string &)
{
    throw std::runtime_error("CylonHnsw: saveIndex() not supported "
                             "(index lives on the device)");
}

std::priority_queue<std::pair<float, hnswlib::labeltype>>
CylonHnsw::searchKnn(const void *query_data, size_t k,
                     hnswlib::BaseFilterFunctor *) const
{
    if (!ctx_) {
        throw std::runtime_error("CylonHnsw: context closed");
    }
    if (k < 1 || k > CYLON_KMAX) {
        throw std::runtime_error("CylonHnsw: k out of range 1..64");
    }
    uint32_t ids[CYLON_KMAX];
    float dists[CYLON_KMAX];
    cylon_status rc = cylon_search(ctx_, CYLON_PREC_F32, query_data, 1,
                                   (uint32_t)k, 0, ids, dists, &stats_);
    if (rc != CYLON_ST_OK) {
        throw std::runtime_error(std::string("CylonHnsw: search: ") +
                                 cylon_strerror(rc));
    }
    std::priority_queue<std::pair<float, hnswlib::labeltype>> pq;
    for (uint32_t j = 0; j < k; j++) {
        if (ids[j] != 0xffffffffu) {
            pq.push({dists[j], (hnswlib::labeltype)ids[j]});
        }
    }
    return pq;
}

uint32_t CylonHnsw::dim() const
{
    return cylon_dim(ctx_);
}

uint64_t CylonHnsw::ntotal() const
{
    return cylon_ntotal(ctx_);
}

cylon_stats CylonHnsw::stats() const
{
    return stats_;
}

} // namespace hnsw_cylon
