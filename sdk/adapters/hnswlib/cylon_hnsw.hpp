// cylon_hnsw.hpp -- hnswlib::AlgorithmInterface<float> adapter over libcylon
// (hnswlib 0.8.0 header-only). A real hnswlib user swaps HierarchicalNSW for
// this class and keeps call sites unchanged: searchKnn returns the same
// max-heap<pair<dist,label>> (pop = farthest first), L2 = squared L2.
#pragma once
#include "hnswlib.h"
#include "cylon.h"

namespace hnsw_cylon {

class CylonHnsw : public hnswlib::AlgorithmInterface<float> {
public:
    explicit CylonHnsw(const cylon_config *cfg);
    ~CylonHnsw() override;

    void addPoint(const void *datapoint, hnswlib::labeltype label,
                  bool replace_deleted = false) override;
    std::priority_queue<std::pair<float, hnswlib::labeltype>>
        searchKnn(const void *query_data, size_t k,
                  hnswlib::BaseFilterFunctor *isIdAllowed = nullptr) const override;
    void saveIndex(const std::string &location) override;

    /* cylon extras: the interface carries no ntotal/dim */
    uint32_t dim() const;
    uint64_t ntotal() const;
    cylon_stats stats() const;

private:
    cylon_ctx *ctx_ = nullptr;
    cylon_config cfg_{};
    mutable cylon_stats stats_{};
};

} // namespace hnsw_cylon
