# Cylon SDK Tutorial — 用 libcylon 在 CXL-SSD 加速器上跑 ANNS

本文是 libcylon 的快速上手走查（论文 artifact 的 tutorial 骨架），覆盖三种
接入方式：原生 C API、FAISS 适配器、Python (ctypes) 绑定。设备 = Cylon
CXL-SSD 模拟器（FEMU/QEMU）；假设 blob（CYH1 格式索引）已由宿主建图管线生成。

## 0. 环境自检

```bash
ls /dev/dax0.0            # 设备窗口存在（QEMU devdax）
cat /proc/mounts | grep dax   # devdax 挂载形态
```

设备档案自述（无需先 load）：`cylon_info` 返回 dim / ntotal / storage_prec /
accum_prec / metric / in_prec_mask —— SDK 永不静默降级：不支持的输入精度返回
`CYLON_ST_EPREC`，越界的 k 返回 EINVAL 类错误，绝不悄悄换路。

## 1. C API（c_min 形态，~20 行）

```c
#include "cylon.h"

cylon_config cfg = {};
cfg.blob_path = "cyh1_wiki.blob";   /* CYH1 blob（guest 可见路径） */
cfg.cpu_frac = 0.5;                 /* collab 拆分；NAN = auto-f */
cfg.ef = 100;
cfg.notify = CYLON_NOTIFY_POLL;

cylon_ctx *ctx;
cylon_open(&ctx, &cfg);             /* flock 单 ctx；EBUSY = 已被占用 */
cylon_load(ctx);                    /* ping→FLUSH→staging→BIND，一次到位 */
cylon_dim(ctx);                     /* 768 */
cylon_ntotal(ctx);                  /* 21015300（wiki_dpr_e5） */

float dist[10];
uint32_t ids[10];
cylon_search(ctx, CYLON_PREC_F16, q_fp16, 1, 10, 100, ids, dist, &stats);
/* stats.f_cur = 本批实际生效的拆分；计数器 = 引擎原样透传 */

cylon_close(ctx);                   /* 释放窗口锁与堆内存 */
```

## 2. FAISS 用户：换索引即换后端

```cpp
#include "cylon_index.hpp"   // faiss_cylon::CylonIndex : faiss::Index

cylon_config cfg = {};
cfg.blob_path = "cyh1_wiki.blob";
cfg.cpu_frac = 0.5;
faiss_cylon::CylonIndex index(768, &cfg);   /* 构造 = open + load */
index.search(nq, x_fp32, k, distances, labels);  /* 标准 faiss::Index 语义 */
/* add()/reset() 拒绝：CYH1 是预建索引，建图走宿主建图管线 */
```

FAISS 用户代码零修改（构造参数换成 CylonIndex 即可），fp32 输入在库内
RNE 转成器件 F16 档案（info.in_prec_mask 报告可接受精度）。

## 2b. hnswlib 用户：AlgorithmInterface 适配器

```cpp
#include "cylon_hnsw.hpp"   // hnsw_cylon::CylonHnsw : hnswlib::AlgorithmInterface<float>

cylon_config cfg = {};
cfg.blob_path = "cyh1_wiki.blob";
cfg.cpu_frac = 0.5;
hnsw_cylon::CylonHnsw index(&cfg);       // 构造 = cylon_open + cylon_load
auto pq = index.searchKnn(q_fp32, k);    // 标准 hnswlib 语义（max-heap，pop=最远）
/* addPoint()/saveIndex() 拒绝：CYH1 是预建索引，建图走宿主建图管线 */
```

头文件适配器 + 一个 .cpp（头文件依赖 hnswlib 0.8.0 sdist；`make hnsw_demo
HNSW_INC=...`）。`searchKnn` 逐查询走 C ABI（单查询 job），返回与 hnswlib
一致的 `priority_queue<pair<dist,label>>`；padding 槽（0xffffffff）不入堆。
CylonHnsw 析构 = cylon_close（RAII 语义与 faiss 适配器一致）。
**逐查询语义**：searchKnn 每次调用 nq=1，collab 拆分退化为全 CPU
（f×nq 四舍五入；nq=1 时 n_cpu=1）——正确性与 engref 等价（结果与 f 无关），
但引擎不参与单查询作业；吞吐叙事属于批处理用户（§1/§2 的整批 search）。

## 3. Python 用户：ctypes 绑定

```python
from cylon import Cylon

with Cylon(blob_path="cyh1_wiki.blob", cpu_frac=0.5, ef=100) as c:
    info = c.info()                 # 设备档案自述
    D, L = c.search(queries_fp32, k=10, ef=100)   # numpy (nq,768) fp32
    c.last_stats["f_cur"]           # 本批生效拆分
```

零依赖（stdlib + numpy）；`CYLON_LIB` 环境变量选 libcylon.so 路径。

## 4. auto-f（可选）

`cpu_frac = NAN`（C）或 （Python 侧传 float('nan')）开启
max-model 自动拆分：每批次 f ← t_e/(t_c+t_e)，clamp [0.05,0.95]；每批的生效
值经 `stats.f_cur` 披露。M3 验收：auto 3 批内收敛到 0.5±0.1（见 CYLON-SDK.md
状态行）。

## 5. 约束表（v1）

| 约束 | 值/语义 |
|---|---|
| 度量 | L2²（fp32 累加，fp16 存储） |
| k | 1..64（CYLON_KMAX） |
| 单 ctx | flock /tmp/cylon-window.lock，第二 ctx = EBUSY |
| 索引 | CYH1 预建（add()/reset() 拒绝） |
| 输入精度 | F32（库内 RNE 转 F16）或 F16 直通；其他 = EPREC |
| auto-f | cpu_frac=NAN；批次级更新；f 经 stats.f_cur 披露 |
| 等待模式 | POLL（v1 实验语义）；AUTO/DOORBELL 预留 |
| staging | load() 自动（dev 优先 21s，ft 回退）；重跑无需手工 FLUSH |
| 崩溃族 | b250 臂客户端概率性 GPF/SEGV；重试存活，数据确定性不受影响 |

## 6. 验收锚点（本 tutorial 的三语言 + auto-f + hnswlib 验收）

- blob: cyh1_wiki.blob（wiki_dpr_e5 CYH1 21M×768d）；1000 条 NQ 查询，k=10, ef=100
- **f 扫描（b250/1000q，BI knob=250）四臂 dump 全部逐字节 == engref**（数据与 f 无关）：
  f=0.25: wall 160.8s, dist 3529216 / hops 85783, engine_ns 160.6s（引擎瓶颈）
  f=0.50: wall 113.5s, dist 2363339 / hops 57302, engine_ns 113.3s（引擎瓶颈，近最优）
  f=0.65: wall 135.6s, dist 1657656 / hops 40210, engine_ns 80.6s（CPU 瓶颈：650×0.207s）
  auto:  3 批 f 0.5→0.525→0.522 CONVERGED，总 wall 112.7s ≈ f=0.5 固定臂（噪声内）
- 三语言 dump 逐字节 == engref_ef100.dump（M2 门禁）；auto-f 每批 stats.f_cur 回读一致
- **hnswlib（M3-2）**：CylonHnsw 逐查询 searchKnn wall 210.7s（nq=1 → 全 CPU 退化，
  引擎计数全 0，见 §2b）；**集合级门禁 1000/1000 行**（m + 升序 ids + pad 规范化 vs
  engref）PASS —— 逐查询/批处理两种调用形态、两种结果表示（提交序/升序）同源同正确
