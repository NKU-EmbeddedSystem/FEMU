# CYLON-SDK 设计 — 用户态加速库：让 CXL-SSD 加速器可被方便使用与集成

状态：**v1.4（2026-09-08）M1+M2 已落码部署，门禁全过**：sdk/（libcylon.so 双档 + 薄
CLI + c_min C API 例子 + faiss 适配器 CylonIndex + ctypes Python 绑定 + tests）repo +
guest /tmp/sdk 均构建通过；C ABI = §3（search() 行内 [m,k) padding 语义见决策 2，m
可重建）。M1 门禁矩阵（全 dump 逐字节 == engref ec3059fbb6561f67fb2b2402603fbf3c）：
CONTROL 臂 scalar CLI（wall 116.28s，本臂新锚）+ b250 scalar CLI ×2（113.67 带内 /
117.18 噪声出带）+ b250 avx CLI（113.40 带内）+ c_min C API（b250）逐字节过，引擎
计数逐位归位。**M2 门禁三连（b250 臂，BI knob=250）**：c_min (F16 C API) / faiss_demo
(F32 适配器) / py_demo (F32 ctypes) 三 dump 全部逐字节 == engref；引擎 dist 2363339 /
hops 57302 三客户端全程位等（pages 抖动 = BI retrap 时序）；适配开销（ext-wall vs
c_min 同臂）：faiss −3.7% / py −4.0%（负值 = 噪声内 <5% ✓）。事故记录：M1 = 手写
stage_buf memcpy 源指针步进翻倍 + 堆越界读（教训 = 手写数据面必审指针算术）；M2 =
①cylon.py 把 u32 标签流写进 uint64 numpy 缓冲（stride-2 交错混排，修复 = u32 缓冲；
教训 = ctypes 缓冲 dtype 必须逐字段对照 ABI）②faiss_demo fp16→fp32 升位 subnormal
归一化 ex 初值差 2（值恒缩 4×，860/768000 个查询分量，2 行并列被微扰翻转；教训 =
手写浮点转换必对 numpy 位级对拍）。A2/B 首跑 GPF + M2 faiss_demo 首跑 SIGSEGV =
崩溃族概率性复现（重试存活，数据确定性不受影响）。四决策不变：①cpu_search
平移进 sdk/（搬家而非复制，splitter = sdk/tools/split_cpu_search.py）；②适配器
FAISS 先行；③建图工具不入范围；④精度 = 接口按"一类加速器"设计（输入精度可选
枚举含预留档案 + 设备档案 info 自述 + 损失明示/re-rank 无损路径）。
hdm-db 旋钮缺省 off（Type-2 叙事已冻结，见 CYLON-TYPE2.md）。
前置叙事：Type-2 roadmap（CYLON-TYPE2.md）已收官（D1-D4 推送 origin/dev-cxl-type2）；
本工作流回到 **Type-3 加速器故事**——设备语义回到"Type-3 + 控制器内算力"，
hdm-db 旋钮缺省 off。目标：把今天"实验客户端"的使用方式升级为**可被第三方
应用与计算库接入的用户态库**。

## 0. 一句话目标

把"mmap devdax + 手工信箱协议 + 手工 staging + 手工 collab 拆分"的实验客户端，
封装成 **libcylon**：一次 `load()`、一次 `search()`；并给 FAISS / hnswlib /
Python 各一层薄适配器，让现有 ANNS 用户零协议知识接入。

## 1. 现状盘点：今天用户要自己碰的东西（= SDK 要消化的东西）

以 cpu_search.c（1028 行）为参照，一个用户今天要：

| 步骤 | 现状 | SDK 化后 |
|---|---|---|
| 打开窗口 | 手工 open+mmap /dev/dax0.0，sysfs 探大小，信箱指针 = win+sz-4096 | `cylon_open()` 一行 |
| 清态 | ping + PNM_OP_CACHE_FLUSH（重跑防 stale EPTE，#27） | load() 内自动 |
| 灌索引 | stage_file 33min first-touch（冷 boot）或 `--stage=dev` 21s；fresh boot 覆盖 0 回退协议；`-S` 假设已驻留 | load() 自动：dev 优先、ft 回退、覆盖校验 |
| 绑定 | PNM_OP_BIND_INDEX + 120s 超时 | load() 内自动 |
| 查询灌窗 | 查询文件 stage 到 blob 尾页对齐处；引擎从窗口读查询向量 | search() 内部把用户 buffer 拷入窗口查询区 |
| 提交搜索 | 手工信箱 v2：gen 字、单 outstanding、poll/sleep/doorbell 三等待模式 | 库内封装，doorbell 自动探测 |
| collab 拆分 | `-F f` 手工拆 CPU/引擎份 + pthread worker + feeder | 库内线程池，f 为 config |
| 结果解析 | 从窗口结果区逐条 memcpy（8B = id+dist，n_found@k*8） | search() 直接填用户数组 |
| 确定性对拍 | gt/dump/verify_window/engref 门禁 | **实验概念不进 SDK**；由对拍工具承担 |

## 2. 分层架构

```
应用 / FAISS 适配器 / hnswlib 适配器 / Python
     │
┌────▼─────────────────────────────────────────┐
│ 适配层（薄）：faiss::Index 子类 / hnswlib        │
│   AlgorithmInterface / numpy ctypes           │
├───────────────────────────────────────────────┤
│ C++ 层：cylon::Index（RAII + 异常/错误映射）      │
├──────────────────────────────────────────────┤
│ C ABI：libcylon.so + cylon.h（唯一真相源）       │
├───────────────────────────────────────────────┤
│ 平台层：窗口 mmap / staging / 信箱 v2 / doorbell │
│   / collab 线程池 / fp16 转换 —— 今日 cpu_search │
│   内核平移，引擎（pnm.c）与 ABI（pnm_uapi.h）零改动 │
└───────────────────────────────────────────────└
```

**原则**：引擎与信箱 ABI 不动（E1''/E-M 数据可比性冻结）；SDK 全部增量在客户端
侧；实验工具链（cpu_search/wiki_exp/engref 门禁）与 SDK 共享同一遍历源码，由
逐字节门禁保护等价性。

### 2.1 单一真相源：cpu_search.c 吸收进 SDK

遍历核心（pnm_search/heap/visit/canonical tie-break）、信箱层（mb_submit v2）、
collab 线程模型**整体平移**进 `sdk/`，cpu_search.c 变为 SDK 上的薄 CLI（保留全部
实验旗标：-g/-o/-S/--stage/--notify/-F/-T/-R）。这样：

- wiki_exp.sh 一行不改照跑；历史数据可比性由**逐字节门禁**守护（SDK 化重构
  client 侧，引擎零改动，正对"只改时序/路径不改数据"的门禁适用面）；
- SDK 与实验工具永不分叉（防 #38/#50 式双源漂移）；
- 平移后的等价性验收 = f=0.5 锚点 + E1'' 关键点复跑 dump 逐字节。

**反方向（copy 而不平移）被否**：两个 1000 行级客户端并存，漂移只是时间问题。

## 3. C ABI 设计（唯一真相源）

```c
/* sdk/include/cylon.h — plain C, <stdint.h> only */
typedef struct cylon_ctx cylon_ctx;

typedef enum {
    CYLON_ST_OK = 0,
    CYLON_ST_EINVAL,      /* 参数/配置非法 */
    CYLON_ST_ENODEV,      /* 窗口/门铃设备不可达 */
    CYLON_ST_ETO,         /* 引擎超时/状态机失效（需 re-open） */
    CYLON_ST_EAVX,        /* avx 构建 × f=0.25 毒点组合，显式拒绝 */
    CYLON_ST_EPREC        /* 请求的输入精度不在能力集内（info.in_prec_mask） */
} cylon_status;

typedef enum {                     /* 等待模式（E-M 三臂转正为 config） */
    CYLON_NOTIFY_AUTO = 0,         /* /dev/cylon-db 在 → doorbell，否则 poll */
    CYLON_NOTIFY_POLL,             /* v1 紧自旋（实验对照用） */
    CYLON_NOTIFY_DOORBELL
} cylon_notify_mode;

typedef struct {
    const char *window_dev;        /* NULL = "/dev/dax0.0" */
    const char *blob_path;         /* CYH1 blob（load 时用） */
    double      cpu_frac;          /* collab 拆分；NAN = auto（见 §4.2） */
    uint32_t    n_cpu_threads;     /* 0 = auto（默认半数在线核） */
    uint32_t    ef;                /* 默认 100，search 可逐调用覆盖 */
    cylon_notify_mode notify;
    uint32_t    stage_bps;         /* 0 = 引擎缺省计费（2GB/s） */
} cylon_config;

typedef struct {                   /* 只读统计（engine resp 直通） */
    uint64_t n_dist, n_hops, n_pages, engine_ns;
} cylon_stats;

/* 精度 = 搜索接口的一等参数。精度命名空间按"一类加速器"设计（本模拟器只是第一个
 * 实现）：设备支持什么，经 info 查询；接口不锚定任何单一档案 */
typedef enum {
    CYLON_PREC_F32  = 0,     /* 通用入口：库内转换到设备存储精度 */
    CYLON_PREC_F16,          /* 半精度直通 */
    CYLON_PREC_BF16,         /* 保留：bfloat16 档案 */
    CYLON_PREC_F64,          /* 保留：双精度档案 */
    CYLON_PREC_I8            /* 保留：量化档案（scale/zero-point 语义另定） */
} cylon_prec;

#define CYLON_PREC_BIT(p)  (1u << (p))

/* 能力与精度披露（open 后即可查，不依赖 load）。契约 = 设备自述，SDK 不硬编码：
 * 本模拟器 v1 档案 = storage F16 / accum F32 / metric l2；换引擎/换 v2 blob，
 * 同一接口按其自述分发 */
typedef struct {
    uint32_t dim;
    uint64_t ntotal;
    cylon_prec storage_prec;     /* 索引向量存储精度（量化损失主导项） */
    cylon_prec accum_prec;       /* 距离累加精度 */
    const char *metric;          /* "l2" */
    uint32_t in_prec_mask;       /* 可接受的输入精度位集（库能转换到 storage 的全部） */
} cylon_info;

/* 生命周期 */
cylon_status cylon_open (cylon_ctx **out, const cylon_config *cfg);
cylon_status cylon_load (cylon_ctx *ctx);            /* FLUSH+stage+BIND */
cylon_status cylon_search(cylon_ctx *ctx,
                          cylon_prec prec,
                          const void *queries, uint32_t nq,   /* (nq,dim)，按 prec 解释 */
                          uint32_t k, uint32_t ef,            /* ef=0 用 ctx 缺省 */
                          uint32_t *out_ids, float *out_dist, /* (nq,k) 行主 */
                          cylon_stats *stats /* 可 NULL */);
cylon_status cylon_close(cylon_ctx *ctx);

/* 披露 */
cylon_status cylon_get_info(const cylon_ctx *, cylon_info *out);

/* 辅助（info 便捷封装） */
uint32_t cylon_dim(const cylon_ctx *);      /* blob header 直读 */
uint64_t cylon_ntotal(const cylon_ctx *);
```

**语义决策**：

1. **精度语义（输入可选、档案自述、损失明示）**——接口按"一类 CXL-SSD 加速器"设计，
   不锚定本模拟器实现：
   - **输入精度可选**：`search()` 的 prec 参数。库负责把输入转换到设备存储精度
     （本档案：F32→F16 每查询舍入一次，相对误差 ~2^-11；F16 直通）。能力集经
     `info.in_prec_mask` 查询，越界请求 = EPREC，**不静默降精度**。
   - **计算/存储精度 = 设备档案**：经 info 披露（storage_prec/accum_prec/metric），
     SDK 不硬编码。本模拟器 v1 档案 = **fp16 载体 + fp32 累加 + L2**（pnm.c
     pnm_dist：f16→f32 展开、fp32 累加；查询读入本就是 fp16，pnm.c:655）。SDK
     不假装可选——假装可选才是隐瞒损失。
   - **损失明示**：损失 = 存储量化（主导）+ 输入舍入；本档案表现为距离 ~5e-4 相对
     误差、recall 0.9720@ef100（wiki 21M×768d vs fp32 精确 GT）。**无损用法写入
     文档**：拿返回 ID 宿主 fp32 re-rank（gather + k×dim L2，微秒级/查询）。
   - **档案扩展路径**：CYH1 v2 头部加显式 precision 字段 → 同一 SDK 按头分发新档案
     （bf16/int8/fp64 枚举位已预留）；真实器件不同硅档 = 换档案不换接口。
2. **结果 ID 语义**：= CYH1 label 空间（建图导出已做 label 重映射，见
   hnswlib-label-permutation 记忆）；空槽规范 = id=0xFFFFFFFF + dist=0.0f；
   search() 输出行内 **[m,k) 段库填 padding：id=0xFFFFFFFF + dist=0.0f**
   （与现行客户端 g_all 归一化一致），因此 m 在调用方**可精确重建**（非
   0xFFFFFFFF 项计数即可，真实 label < 2^24 永不撞 padding 值）——实验字节
   门禁可以走公开 API 完成。适配器层对 padding 原样透传。
3. **stats 透传**：n_dist/n_hops/n_pages/total_ns 是引擎 resp 原语，论文实验
   （misses→wall 预测、BI/atomic 计费叙事）要原样拿到，不加工。注意：计数是
   **引擎臂份额**（CPU 臂不产生引擎 resp），随 f 变化，不是全批次量。
4. **错误模型**：不设回调、不抛异常过 ABI；C++ 层映射成异常，Python 层映射成
   异常。引擎超时返回 ETO 且 ctx 置毒，须 close+open（与现行客户端语义一致：
   不做引擎内重试，超时 = 环境异常）。

## 4. 平台层语义（SDK 内部消化的运行时约束）

### 4.1 生命周期自动化

- **load() 序列** = ping → CACHE_FLUSH → staging（dev 优先：PNM_OP_STAGE，覆盖
  shortfall/ENOSYS/覆盖≠want → FLUSH + first-touch 回退）→ magic/头校验 → BIND。
  staging 策略 = `CYLON_STAGE_AUTO`（dev 优先 ft 回退，即现行 stage_dev 的回退
  协议转正为缺省）；不暴露 ft/dev 枚举给普通用户，实验旗标留在 CLI 层。
- **跨进程恢复 = 协议自愈（engine 零改动）**：v2 信箱协议内建重同步——新进程首个
  job 发 gen=0（cpu_search.c:354-357 现行客户端即如此；引擎 pnm.c:824 对 gen==0
  无条件接受并重新基线化，此后才要求 last+1）。崩溃残留（PENDING/DONE 残留）在
  新进程 open() 的首个 job（NOP ping，gen=0）即被引擎重同步吞掉，无需 seeding。
  v1-first-after-restart（#53）是 v1 冻结工具的操作纪律，对 SDK 客户端结构性消失。
- **doorbell 自动降级**：`/dev/cylon-db` 打不开 → poll，stat 里记录实际模式
  （E-M 式 wall 判别器语义保留在 stats：doorbell 投递死 = wall 膨胀，库不吞）。
- **单客户端语义**：设备同一时刻服务一个 ctx；二次 open 用文件锁
  `/tmp/cylon-window.lock`（flock）防呆，非目标里明确"多客户端并发"不做。

### 4.2 collab 拆分与线程模型

- search(nq) 内部：[0, n_cpu) 给 CPU 侧，[n_cpu, n) 给引擎侧。**M1 保真 = 单
  worker 线程**（与现行 cpu_worker 完全一致：单线程串行走 [0,n_cpu)，遍历态 st
  单线程私有）；feeder = 主线程串行投递（单 outstanding 不变——引擎语义冻结）。
  多 worker 线程池是后续性能项（M3+，非 M1 保真面）。
- **f 缺省 = auto**：首次 search() 用 f=0.5 起步，此后按上次 search 的每查询平均
  时长做批次级更新（max 模型：**f ← t_e/(t_c+t_e)**，t_c/t_e = CPU/引擎臂的每查询
  平均耗时，均衡 = 两臂每查询耗时相等），状态存 ctx。实验复现需要固定 f →
  config 显式给 f 即关 auto。（初稿误写 T_cpu/(T_cpu+T_eng)，已修正。）
- 线程池在 ctx 生命周期内常驻（避免每次 search 建/销毁 pthread）。

### 4.3 窗口内存布局

沿用现行约定（冻结）：blob @0，查询区 = blob 尾页对齐处（现行 qoff 规则），
结果区 = win_sz-PNM_RESULTS_OFF_FROM_END，信箱 = win_sz-PNM_MB_OFF_FROM_END。
窗口查询区容量 = 48GB-blob 尾部，nq 上限 = 剩余空间/dim/2（库内校验）。

## 5. 适配层

### 5.1 FAISS 适配器（M2，优先）

```cpp
// sdk/adapters/faiss/cylon_index.cpp
struct CylonIndex : faiss::Index {
    // d/ntotal/metric_type 来自 blob header（L2 only, fp16 载体）
    // is_trained=true
    void add(idx_t, const float*) override;   // 不支持：CYH1 预建索引
        → 抛 faiss::NotImplementedError（文档指路 builder 工具链）
    void search(idx_t n, const float* x, idx_t k,
                float* distances, idx_t* labels) override {
        // faiss::Index 用 idx_t=int64 labels；CYH1 id 是 u32 label 空间，零损
        cylon_search(ctx_, CYLON_PREC_F32, x, n, k, 0, labels, distances, nullptr);
    }
};
```

- 用户故事：`faiss::Index* idx = new CylonIndex(cfg); idx->search(...)`，或
  Python 侧 `faiss` 现有绑定层直接消费该 Index 子类（faiss python 对自定义
  Index 的包装有现成路径；pybind 实现为 faiss 贡献补丁形态）。
- **hnswlib 适配器**（M3，可选）：实现 `AlgorithmInterface<float>::knn_query`
  薄壳，故事是"CYH1 = hnswlib 图的设备驻留形态"（同一 build 谱系）。
- **Python 绑定**（M2 尾/M3）：ctypes 起步（guest 零依赖，pip 不需要）：
  `libcylon.so` 直接 ctypes；numpy fp32 (nq,dim) → cylon_search 一行映射。
  pybind11/nanobind 升级版等 FAISS 演示落地后再评估。

## 6. 部署与构建

- 落仓库 `sdk/`：`include/cylon.h`、`src/`（core+collab+平台层）、
  `adapters/faiss/`、`bindings/python/`、`tests/`、`examples/`、Makefile
  （**plain gcc**，guest 无 meson/ninja 依赖；宿主侧 Meson 不引入）。
- 部署 = em_p2_bringup.sh 加一步（scp sdk/ → guest 编 .so + 跑对拍），或
  guest 直编。工具链 = guest 内 gcc -O2（与现行客户端同档）。
- 文档：USAGE 新 §（快速上手 + API 参考 + 约束表），TYPE2 文档不回改
  （Type-2 叙事已冻结，SDK 是 Type-3 故事）。

## 7. 已知约束的消化表（SDK 每条对号）

| 现有约束（caveats/记忆） | SDK 消化 |
|---|---|
| v1-first-after-restart（#53，v1 脏槽永拒） | SDK 首帧 gen=0 重同步（协议内建）→ 对 SDK 客户端约束消失（v1 冻结工具纪律保留） |
| 重跑须 FLUSH（#27） | load() 自动 |
| staging 三分法判读（#31） | 覆盖校验 + 自动回退，用户不见 staging 细节 |
| VEX 首触 trap 页雷（#20/#36） | 平台层查询区写入走标量 store（平移现状） |
| avx f=0.25 毒点（#45 修订） | config 校验：avx 构建下 f=0.25 显式拒绝并报 EAVX 毒点；非毒 f 正常 |
| 重启前 sync（#52） | 库不触碰该问题（无 guest 文件写） |
| verify_window 5-8min 税 | SDK 不做窗口 verify（实验工具保留） |
| 48GB 窗口/36GB blob | open() 校验 blob_bytes+查询区 ≤ 窗口；超限 EINVAL |
| fp16 载体 / L2 only / 单索引@0 | cylon_info 披露 + header 直读校验；re-rank 无损用法写入文档 |
| 单 outstanding 信箱 | feeder 串行，吞吐故事 = collab f 而非并发投递 |
| gen 上限 42 亿 job | open() seed 续接，不回绕 |

## 8. 里程碑与验收门禁

**M1（核心库 + 对拍）**：
- cpu_search.c 平移进 sdk/，CLI 薄壳化；wiki_exp.sh 照跑
- 门禁：f=0.5 锚点 + E1'' 关键点（b250_f050/b500_f050/CONTROL）复跑 dump
  薄壳 vs engref 逐字节；SDK C API 路径同点逐字节；wall 带内（±3%）
- 交付：libcylon.so + cylon.h + tests/（对拍工具 dump 与 engref cmp）

**M2（FAISS 适配器 + Python）— 完成（2026-09-08）**：
- CylonIndex(faiss::Index) 子类 + faiss_demo；ctypes Python 绑定 cylon.py + py_demo
- 门禁：FAISS 路径 = Python 路径 = C API 路径 = engref 逐字节三重对拍 ✓
  （b250 臂；引擎计数位等；适配开销 faiss −3.7% / py −4.0%，<5% ✓；
  faiss_demo 首跑 SIGSEGV = 崩溃族，重试存活过门禁）
- 交付：sdk/adapters/faiss/（CylonIndex）+ sdk/examples/faiss_demo.cpp +
  sdk/bindings/python/（cylon.py + py_demo.py）；guest 依赖 = conda-forge libfaiss
  (/tmp/faiss-env, 无 sudo) + apt python3-numpy；pybind/spike 否决 — ctypes 直达
  C ABI（§10 风险项关闭）

**M3（生态故事）**：
- hnswlib 适配器（可选）；auto-f 启发式验收（f 扫描对照：auto 收敛到 0.5±0.1
  3 次内）；教程/示例 notebook 形态的论文 artifact 骨架

## 9. 非目标

- 多客户端/多进程并发访问设备（单 ctx 语义；并发是引擎侧新故事）
- 索引构建加速（build 留在宿主 builder；SDK 只消费 CYH1）
- 引擎/信箱 ABI 任何改动（E1''/E-M 冻结面）
- 非-L2 度量、fp32 载体索引、压缩索引（PQ/SQ）——CYH1 v1 格式约束；输入精度
  int8/fp64 同理（引擎 v1 单一精度档案，扩档 = 引擎侧 + CYH1 v2 的事）
- faiss 上游贡献补丁（我们先做贡献补丁形态的本地适配器，上游化等论文后）

## 10. 风险与边界

- **平移重构风险**：cpu_search.c → sdk/ 平移的等价性只由逐字节门禁背书；
  若锚点复跑失败，回退方案 = copy 形态（fork 一份进 sdk，实验工具冻结），
  代价是接受双源漂移风险并记录。
- **auto-f**：max 模型在 E1/E2 都内点 ≤5%，但单查询级 T_cpu/T_eng 抖动大 →
  auto 更新用**批次级**（每 search 调用一次更新），并在 stats 里暴露本次 f。
  验收 M3 里给 f 扫描对照。
- ~~**FAISS python 包装自定义 Index 的现成路径**未验证~~ **已关闭（M2）**：跳过
  pybind 包装 faiss::Index 的可行性 spike，直接 ctypes 绑 C ABI（cylon.py）——
  faiss 适配留在 C++ 侧（faiss_demo 可用），Python 用户走 pycylon 原生 API。
- doorbell .ko 缺失时 auto 降级 poll：统计里记录，文档写明（E-M 判别器语义
  保留：投递死 = wall 膨胀 5000s+，库照常返回正确结果）。
- 48GB 单索引上限：多索引 = 引擎侧新故事（非目标）。
