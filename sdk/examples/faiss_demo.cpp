// faiss_demo.cpp -- what a real faiss user program looks like on top of
// the CylonIndex adapter: construct, search, read results. Also serves
// as the M2 byte-gate client (F32 entry; fp16 file upcast exactly).
//
// usage: faiss_demo -f blob -q queries_fp16.bin -o dump [-d 768] [-k 10]
//        [-n 1000] [-e 100] [-w /dev/dax0.0]
#include "cylon_index.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

static uint16_t *read_file(const char *path, size_t *nbytes)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror(path);
        exit(1);
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint16_t *buf = (uint16_t *)malloc(sz);
    if (!buf || fread(buf, 1, sz, fp) != (size_t)sz) {
        fprintf(stderr, "faiss_demo: read %s failed\n", path);
        exit(1);
    }
    fclose(fp);
    *nbytes = (size_t)sz;
    return buf;
}

int main(int argc, char **argv)
{
    const char *blob = NULL, *qfile = NULL, *dumpfile = NULL;
    int dim = 768, k = 10, nq = 1000, ef = 100;
    cylon_config cfg = {};
    cfg.cpu_frac = 0.5;                 /* fixed f: anchor-arm semantics */
    cfg.ef = 100;
    cfg.notify = CYLON_NOTIFY_POLL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) blob = argv[++i];
        else if (!strcmp(argv[i], "-q")) qfile = argv[++i];
        else if (!strcmp(argv[i], "-o")) dumpfile = argv[++i];
        else if (!strcmp(argv[i], "-d")) dim = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-k")) k = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n")) nq = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-e")) ef = cfg.ef = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w")) cfg.window_dev = argv[++i];
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 1; }
    }

    cfg.blob_path = blob;
    if (!blob || !qfile || !dumpfile) {
        fprintf(stderr, "usage: faiss_demo -f blob -q queries -o dump "
                "[-d 768] [-k 10] [-n 1000] [-e 100] [-w dev]\n");
        return 1;
    }
    size_t qbytes = 0;
    uint16_t *q16 = read_file(qfile, &qbytes);
    if (qbytes < (size_t)nq * dim * 2) {
        fprintf(stderr, "faiss_demo: query file too small\n");
        return 1;
    }
    /* exact fp16 -> fp32 upcast: the demo's "user data" is fp32, the
     * device contract converts back with one RNE rounding (identity for
     * values that started as fp16) */
    std::vector<float> x((size_t)nq * dim);
    for (size_t i = 0; i < x.size(); i++) {
        uint32_t s = q16[i] & 0x8000u, e = (q16[i] >> 10) & 0x1fu,
                 m = q16[i] & 0x3ffu;
        uint32_t bits;
        if (e == 0) {
            /* fp16 subnormal/zero -> fp32 (exact) */
            if (m == 0) bits = s << 16;
            else {
                int ex = 1;   /* E = 113 - shifts (fp16 subnorm m*2^-24;  */
                              /* ex=-1 here gives 4x-too-small values)   */
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

    /* construct = open + load (device staging + BIND happen here) */
    faiss_cylon::CylonIndex index(dim, &cfg);
    cylon_info info;
    cylon_get_info(index.handle(), &info);
    printf("device: dim %u ntotal %lu storage f16 accum f32 metric %s "
           "in_prec_mask 0x%x\n", info.dim, (unsigned long)info.ntotal,
           info.metric, info.in_prec_mask);

    std::vector<float> D((size_t)nq * k);
    std::vector<faiss::idx_t> L((size_t)nq * k);
    auto t0 = std::chrono::steady_clock::now();
    index.search(nq, x.data(), k, D.data(), L.data());
    double wall = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    cylon_stats st = index.stats();
    printf("engine: dist %lu hops %lu pages %lu ns %lu\n",
           (unsigned long)st.n_dist, (unsigned long)st.n_hops,
           (unsigned long)st.n_pages, (unsigned long)st.engine_ns);
    printf("faiss search wall: %.3f s\n", wall);

    /* byte-gate dump: same format as the experiment clients (u32 m +
     * k x u32 ids per row; m = non-padding count) */
    FILE *dfp = fopen(dumpfile, "wb");
    if (!dfp) {
        perror(dumpfile);
        return 1;
    }
    for (int qi = 0; qi < nq; qi++) {
        uint32_t m = 0;
        for (int j = 0; j < k; j++) {
            if ((uint32_t)L[(size_t)qi * k + j] != 0xffffffffu) {
                m++;
            }
        }
        fwrite(&m, 4, 1, dfp);
        for (int j = 0; j < k; j++) {
            uint32_t id = (uint32_t)L[(size_t)qi * k + j];
            fwrite(&id, 4, 1, dfp);
        }
    }
    fclose(dfp);
    printf("dump written: %s (%ld B)\n", dumpfile,
           (long)((size_t)nq * (4 + 4 * k)));
    return 0;
}
