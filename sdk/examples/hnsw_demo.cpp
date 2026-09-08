// hnsw_demo.cpp -- M3 gate client for the hnswlib adapter: a real hnswlib
// user shape (searchKnn one query at a time), results canonicalized
// per row as m + k x u32 ids sorted ascending (padding 0xffffffff last)
// so the byte gate compares order-insensitively against engref.
#include "cylon_hnsw.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <string>
#include <vector>

static uint16_t *read_file(const char *path, size_t *nbytes)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); exit(1); }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint16_t *buf = (uint16_t *)malloc(sz);
    if (!buf || fread(buf, 1, sz, fp) != (size_t)sz) {
        fprintf(stderr, "hnsw_demo: read %s failed\n", path);
        exit(1);
    }
    fclose(fp);
    *nbytes = (size_t)sz;
    return buf;
}

static void upcast(const uint16_t *q16, size_t n, float *x)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t s = q16[i] & 0x8000u, e = (q16[i] >> 10) & 0x1fu,
                 m = q16[i] & 0x3ffu;
        uint32_t bits;
        if (e == 0) {
            if (m == 0) { bits = s << 16; }
            else {
                int ex = 1;   /* E = 113 - shifts (M2 upcast fix) */
                uint32_t mm = m;
                while (!(mm & 0x400u)) { mm <<= 1; ex--; }
                mm &= 0x3ffu;
                bits = (s << 16) | ((uint32_t)(127 - 15 + ex) << 23) |
                       (mm << 13);
            }
        } else if (e == 31) {
            bits = (s << 16) | 0x7f800000u | (m << 13);
        } else {
            bits = (s << 16) | ((e - 15 + 127) << 23) | (m << 13);
        }
        memcpy(&x[i], &bits, 4);
    }
}

int main(int argc, char **argv)
{
    const char *blob = NULL, *qfile = NULL, *dumpfile = NULL;
    int dim = 768, k = 10;
    size_t nq = 1000;
    uint32_t ef = 100;
    cylon_config cfg = {};
    cfg.cpu_frac = 0.5;
    cfg.ef = 100;
    cfg.notify = CYLON_NOTIFY_POLL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) blob = argv[++i];
        else if (!strcmp(argv[i], "-q")) qfile = argv[++i];
        else if (!strcmp(argv[i], "-o")) dumpfile = argv[++i];
        else if (!strcmp(argv[i], "-d")) dim = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-k")) k = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n")) nq = strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-e")) { ef = strtoul(argv[++i], 0, 0); cfg.ef = ef; }
        else if (!strcmp(argv[i], "-w")) cfg.window_dev = argv[++i];
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 1; }
    }
    cfg.blob_path = blob;
    if (!blob || !qfile || !dumpfile) {
        fprintf(stderr, "usage: hnsw_demo -f blob -q q_fp16 -o dump "
                "[-d 768] [-k 10] [-n 1000] [-e 100] [-w dev]\n");
        return 1;
    }
    size_t qbytes = 0;
    uint16_t *q16 = read_file(qfile, &qbytes);
    if (qbytes < (size_t)nq * dim * 2) {
        fprintf(stderr, "hnsw_demo: query file too small\n");
        return 1;
    }
    std::vector<float> x((size_t)nq * dim);
    upcast(q16, (size_t)nq * dim, x.data());
    /* construct = open + load (staging + BIND inside) */
    hnsw_cylon::CylonHnsw index(&cfg);
    printf("device: dim %u ntotal %lu\\n", index.dim(),
           (unsigned long)index.ntotal());
    FILE *dfp = fopen(dumpfile, "wb");
    if (!dfp) { perror(dumpfile); return 1; }
    auto t0 = std::chrono::steady_clock::now();
    for (size_t qi = 0; qi < nq; qi++) {
        auto pq = index.searchKnn(x.data() + qi * dim, k);
        uint32_t m = (uint32_t)pq.size();
        fwrite(&m, 4, 1, dfp);
        std::vector<uint32_t> row;
        while (!pq.empty()) {
            row.push_back((uint32_t)pq.top().second);
            pq.pop();
        }
        std::sort(row.begin(), row.end());
        size_t w = 0;
        for (uint32_t v : row) { fwrite(&v, 4, 1, dfp); w++; }
        uint32_t pad = 0xffffffffu;
        for (; w < (size_t)k; w++) { fwrite(&pad, 4, 1, dfp); }
    }
    double wall = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    fclose(dfp);
    cylon_stats st = index.stats();
    printf("hnsw search wall: %.3f s (%zu queries, k=%d)\\n", wall, nq, k);
    printf("engine: dist %lu hops %lu pages %lu ns %lu f=%.3f\\n",
           (unsigned long)st.n_dist, (unsigned long)st.n_hops,
           (unsigned long)st.n_pages, (unsigned long)st.engine_ns, st.f_cur);
    printf("dump written: %s\\n", dumpfile);
    return 0;
}
