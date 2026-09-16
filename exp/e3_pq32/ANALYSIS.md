# E3 — PQ32×8 引擎 vs CPU+swap baseline 对比（wiki_dpr_e5 21M×768d）

用户 directive 三步：(a) 构造召回可用的 PQ 配置 → (b) 纯 CPU+swap baseline（速度+精度）→ (c) CXL SSD 加速器能否进一步提速。

## E3-A — 配置门禁（已完成）

- PQ32×8（32 子空间 × 256 质心 × 24 维），A1 布局，ef=R=2048。
- ref searcher 门禁：recall@10 = **0.9458**（n=1000，≥0.90 达标；PQ16@2048 仅 0.611）。
- blob：aisaq_pq32_a1.blob 38,518,058,728B；导出时 off_codes 少 8B（已知"+40 残留"复发），头 @80 原位 +8 修复，md5 = 6b7b77c04206f7991b1df6bc2cabccd3。


## E3-B — 纯 CPU+swap baseline（宿主侧，cgroup v2 内存上限）

机制：mmap 38.5GB blob 于 sda HDD + `systemd-run --user -p MemoryMax=CAP` 限内存 + posix_fadvise(DONTNEED) 冷启动。页缓存被驱逐 ⇒ 每次重排序都从盘上随机取页（swap 语义）。墙钟为外部 date 采样（clock() 不含 I/O 等待）。

| 档位 | n | 每查询 | recall@10 | dist/hops（每查询） | dump md5 |
|---|---|---|---|---|---|
| 40G 全驻（无约束） | 1000 | **19 ms** | 0.9458 | 91,382.4 / 2,061.9 | d8986bce… |
| 24G cap | 100 | **11,493 ms** | 0.9630* | 89,806.4 / 2,060.8 | 5948a743… |
| 12G cap | 100 | **11,394 ms** | 0.9630* | 89,806.4 / 2,060.8 | 5948a743… |

*cap 档只跑 n=100（11.4s/q ⇒ n=1000 需 3.2h）。recall 略高于 n=1000 是小样本波动；dist/hops 与全驻档同族（同 walk）。
- 每遍 pass 数值两两完全一致（确定性）；40G 全驻两遍 19ms/q；HDD 随机页取数 ≈ 194 IOPS ⇒ 单查询 ~900 页流 ⇒ ~11.4s/q，与 cap 档实测一致；24G/12G 不可区分（重排序流主导）。
- **基线结论：容量一受限，纯 CPU+swap 从 19ms 崩到 11.4s（~600×），被 HDD 随机页延迟钉死。**

## E3-C — 引擎臂（CXL SSD 加速器）

链路：blob rsync → guest（md5 一致 6b7b77c0…）→ SDK 重部署 → first-touch staging 全 blob（36,733MiB，2,074s ≈ 17.7MB/s；D2 dev-staging covered 0/9,403,823 页——FTL maptable 重启后为空、guest 文件在 qcow2 启动盘上从未经器件写路径映射，符合设计，客户端自动回退 first-touch）→ BIND（codes 区 164,185 页入器件缓存）→ 搜索。

### 臂 1：器件缓存 512MB（默认 bufsz=512）

| n | exec/查询 | p50 | misses/查询 | recall@10 | dist/hops | dump md5 |
|---|---|---|---|---|---|---|
| 1000 | **873.8 ms** | 756.5 ms | 20,072.9 | **0.9458** | 91,382.4 / 2,061.9 | **d8986bce…（== baseline 40G 逐字节一致）** |

engine/query 细分：rerank 2048.0，code-pages（codes 区 miss）15,026.2，vec-pages（重排序向量 miss）2,814.4。

**根因（配置失配，非算法问题）**：bufsz=512（器件缓存 512MB）是 SIFT1M 时代的默认值；PQ32 codes 区 672MB > 512MB，BIND 钉不住，ADC 逐子空间读码持续 miss。FEMU 日志逐 job：misses 10K-60K，exec 与 miss 强相关（≈40µs/miss ≈ pg_rd_lat）。telemetry 20,072 ≈ adj 2,062 + codes 15,026 + vec 2,814。

### 臂 2：器件缓存 2GB（/tmp/femu-bufsz=2048，宿主 /dev/dax0.0 全量 2.0GB）

（运行中——FEMU 重启 + restage ~35min + BIND + n=1000。）

## 对齐与公平性注记

- **精度完全对齐**：引擎臂 dump 与 baseline 40G 臂逐字节一致（同一 walk ⇒ 同 recall），dist/hops 每查询完全相同。加速器不改变结果，只改变速度。
- **容量语义**：baseline cap 档限制的是宿主 DRAM（页缓存上限，盘 = sda HDD 194 IOPS）；引擎臂的 38.5GB 索引全驻器件侧 48GB tier（devdax DRAM），器件 DRAM 缓存 512MB/2GB 是器件自身资源，不占宿主内存预算。
- **诚实注记（无约束档）**：宿主 123GB 内存全驻时纯 CPU 只要 19ms/q，引擎臂（ mailbox 单 job 串行 + 40µs 级 miss 成本 + 引擎计算）反而慢一个数量级——加速器的价值主张在容量受限部署（索引 >> DRAM），不在全驻场景。
- 传输更正：38.5GB rsync 实测 **~9min（~71MB/s SLIRP）**；此前我误读 guest 时钟（+4.3h 偏移）报成 4.5h，特此更正。
预测：由 G2 分解（exec 264.4 = 计算 ~86ms + 4,547.8 miss × ~39µs ≈ 178ms）与 PQ32 ADC 2× 外推，臂 2 预计 exec ≈ 300-400ms/q；若命中预测则缓存失配定量闭环。E2 同族对照：A2 布局（码内联图页）26,061.8 code-pages/q → 1,865.8ms/q（7.1×），与本臂 512MB 失配同机制。

### 臂 2（修订）：器件缓存 1GB（bufsz=1024）

原计划 2GB（codes 672MB + 余量），实测：**mlock(2GB) 失败 ENOMEM**（"Failed to mlock cache backend (len 2147483648): Cannot allocate memory"）→ 器件缓存整体禁用 → 2GB 臂作废（该轮数据未采用）。边界探测：bufsz=1024 mlock 成功（hpa_base 0x2002200000）。同进程 mlock(48GB 窗口) 却成功—— devdax 映射 mlock 在该内核（6.4.6-cylon）上有 ~1GB 实用上限，**器件缓存被宿主 devdax 容量（/dev/dax0.0 = 2.0GB）与 devdax-mlock 上限双重封顶在 1GB**。已加 strerror 仪表化（cache.c）+ run-cxlssd.sh 加 /tmp/femu-bufsz 覆盖旋钮。

1GB 仍足够覆盖 codes 672MB（钉住 + 352MB 余量给 adj/vec 工作集）。

### 臂 2 实测：器件缓存 1GB（bufsz=1024）

| n | exec/查询 | p50 | misses/查询 | recall@10 | dist/hops | dump md5 |
|---|---|---|---|---|---|---|
| 1000 | **275.9 ms** | 277.9 ms | 4,434.2 | **0.9458** | 91,382.4 / 2,061.9 | **d8986bce…（== baseline 40G 逐字节一致）** |

engine/query 细分：rerank 2048.0，code-pages **0.0**（672MB codes 全 pin），vec-pages 2,577.3，QPS 3.6（p90 300.0ms / p99 313.1ms / max 378.0ms）。

**验证链与模型闭合**：
- dist/hops/rerank 与 baseline 40G 臂逐位一致；dump md5 精确命中 `d8986bceca1755465b2119b422ed9b07` —— 加速器字节级不改变搜索结果。
- miss 分解：20,072.9 → 4,434.2（Δ15,638.7）；Δexec = 873.8 − 275.9 = 597.9ms → **斜率 ≈ 38.2µs/miss**，与 FEMU 日志回归的 ~40µs（≈pg_rd_lat 40µs）吻合。
- 剩余 275.9ms 分解：vec/adj miss 4,434.2 × 38µs ≈ 169ms + ADC/LUT + rerank 计算 ≈ 107ms，闭合。

## E3 终判：三档对比（用户问题：CXL SSD 加速器能否进一步提速 CPU+swap baseline？）

| 部署档 | 资源语义 | exec/查询 | recall@10 | dump md5 |
|---|---|---|---|---|
| CPU 全驻留（123GB 宿主） | 索引 38.5GB 全在宿主 DRAM | **19 ms** | 0.9458 | d8986bce… |
| CPU+swap，cgroup 24G/12G 帽 | 索引 >> DRAM，HDD 随机页回源 | **11,394-11,493 ms** | 0.9630* | 5948a743…（n=100） |
| **CXL SSD 引擎 48G + 器件缓存 1GB** | 索引全驻器件侧 tier，器件 DRAM 1GB 缓存 | **275.9 ms** | 0.9458 | d8986bce…（== 全驻留） |

*受限臂 n=100；* 号臂跑的 walk 与 n=1000 全驻留臂不同（n=100 子集），md5 目标不同属预期。

**结论**：
1. **能，且幅度显著**：容量受限（索引 >> DRAM、随机页回源 HDD）部署里，CXL SSD 引擎臂 275.9ms/q vs 受限 CPU baseline 11.4s/q —— **~41× 提速**，且精度逐字节一致。
2. **不能替代全驻留 CPU**：宿主 DRAM 足够时纯 CPU 19ms/q 仍快 ~14×（mailbox 单 job 串行 + 40µs 级 miss 成本）。加速器的价值主张 = **容量受限部署**（索引 >> DRAM 的单机/边缘场景），与 E1''/E2 叙事一致。
3. **缓存容量是第一阶杠杆**：512MB→1GB（codes 从 miss 风暴到全 pin）单点 3.2×（873.8→275.9ms）。剩余 miss 全在 vec/adj 工作集（4,434/q），下一阶优化方向 = 重排序候选向量预取/pin（预计再省 ~170ms，理论逼近 ~100ms 档）。
4. **工程边界**：devdax mlock ~1GB 实用上限封顶器件缓存；codes 672MB 恰好是 1GB 缓存的甜点，PQ64（1.3GB codes）将需要更大的器件缓存或 A2 内联布局。
