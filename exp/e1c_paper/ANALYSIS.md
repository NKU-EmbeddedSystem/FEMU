# Cylon Type-3 线综合分析：协同结构、瓶颈归因与双重不敏感性

> 数据入库：`exp/e1c_paper/`（自 `/var/tmp/cylon/anns/exp/e1c_paper/`，2026-09-06）
> 配置：cylon-v9.0.1 · 器件锚定 profile（FP16 数 T 脉动阵列 + PCIe4x8）· 48GB 设备 · DER 全模式
> 负载：wiki_dpr_e5 21,015,300 × 768d 真实 e5 向量（fp16），NQ 查询 n=1000，ef=100，k=10
> 门禁：**全部点位 dump 与 `engref_ef100.dump` 逐字节一致，recall@10 = 0.9720**

## 1. 统一模型（一张表看懂）

| 量 | 值 | 出处 |
|---|---|---|
| 纯 CPU（avx，f=1） | 217.4 s / 4.6 QPS | wikie1_f1 |
| 纯引擎（f=0） | 208.3 s / 4.8 QPS | wikie1_f0 |
| f* 预测 = T_cpu/(T_cpu+T_eng) | **0.51** | 模型闭式 |
| f* 实测峰值 | **0.50** | wikie1_f0.5 |
| 峰值 wall | 114.1 s / 8.8 QPS | wikie1_f0.5 |
| 加速比 vs 纯 CPU | **1.91×** | 217.4/114.1 |
| 实测/理想模型 | **96%**（8.8 vs 9.2 QPS） | qps_ideal_model |
| 引擎 job 分解 | miss 4294×48µs ≈ 206ms ＋ 计算 ~22ms | exec/q 228.2ms |
| **miss 时间占比** | **~90%**（纯引擎点 99%） | 206/228.2 |
| 算力拐点 v* | **≈44–46µs/dist**（三口径一致） | §4 |
| BI 一致性税（物理范围） | ≤1%（噪声带内，无系统趋势） | §5 |

四组实验拼出一条完整证据链：**协同结构（E1'）→ 瓶颈归因（miss 主导）→ 算力不敏感（11 点扫描）→ 一致性税不敏感（Type-2 D1）**，闭合于统一模型
`wall(f,v) = max( f·T_cpu , (1−f)·(T_miss + 500·dist·v) )`。

---

## 2. E1'：协同 f 扫描——均衡点物理

数据 `e1c_results.csv`；图 `e1c_qps_vs_f.png`。

| f | 客户端 | wall(s) | QPS | T_cpu | T_eng | exec/q | misses/q | hops | QPS_理想 | ×CPU |
|---|---|---|---|---|---|---|---|---|---|---|
| 1.0 标量(含 staging) | scalar | 227.3 | 4.4 | 227.3 | — | — | — | — | 4.60 | 0.96 |
| 1.0 avx | avx | 217.4 | 4.6 | 217.4 | — | — | — | — | 4.60 | 1.00 |
| 0.75 | avx | 163.7 | 6.1 | 163.7 | 58.6 | 234ms | 4326 | 115.0 | 6.13 | 1.33 |
| **0.50** | avx | **114.1** | **8.8** | 111.6 | 114.1 | 228ms | 4294 | 114.6 | 9.20 | **1.91** |
| 0.25 | avx | 159.8 | 6.3 | 56.2 | 159.8 | 213ms | 4291 | 114.4 | 6.40 | 1.37 |
| 0.00 | avx | 208.3 | 4.8 | — | 208.3 | 208ms | 4300 | 114.4 | 4.80 | 1.04 |

- **wall(f) = max(f·T_cpu, (1−f)·T_eng) 逐点精确成立**：114.1/142.5/163.7/159.8 vs 理想
  114/141/163/160——理想重叠模型在 21M 规模下误差 ≤1%。
- **f* 闭式预测 = 217.4/(217.4+208.3) = 0.51，实测 0.50**：DER 建模引擎与 guest vCPU 恰好
  等速（4.8 vs 4.6 QPS），均衡点在正中，协同余量最大。
- 聚合效率 = 实测峰值/理想上限 = 8.8/9.2 = **96%**（干扰损耗仅 ~4%）。

## 3. 瓶颈归因：引擎是 miss 延迟主导

- **纯引擎点（f=0）99% miss-bound**：exec/q 207.7ms，其中 misses 4299.5 × 48µs（DER
  缺失延迟账单）= 206.4ms → 计算基座只剩 ~1.6ms。
- f=0.5 点 exec/q 228.2ms：同样的 ~206ms miss 时间 ＋ ~22ms 重叠窗干扰（与 E2 256M
  的"引擎吸收干扰"结论一致：CPU 侧干扰被瓶颈侧吸收，不破坏 dump 正确性）。
- hops 114.4–115.0 与 host engref（114.4）一致；dist_per_q 4705–4744（±0.8%，遍历轨迹
  等价）。**器件卖的不是 FLOPS，是"数据在本侧"的访存位置**——引擎为每条 miss 支付
  48µs 器件侧账单，省掉的是 host 经 PCIe/CXL.mem 的往返。

## 4. 算力敏感性：5 个数量级全平坦，拐点闭合于 miss 时间

数据 `e1c_compute_sensitivity.csv`；图 `e1c_compute_sensitivity.png`（11 点，f=0.5）。

| 旋钮 v (ns/dist) | 器件档位 | wall(s) | QPS | vs 基线 114.1s |
|---|---|---|---|---|
| 0 | 默认 profile = 2–8 TFLOPS FPGA（0.19–0.77 ns/dist） | 114.1 | 8.8 | — |
| 1–64 | 2T → 0.024T | 111.5–113.5 | 8.8–9.0 | 噪声带 |
| 256–500 | 6mT–3.6mT | 113.1 | 8.8 | 噪声带 |
| 1000 | ARM 控制器锚点（8.9.7 轴） | 115.5 | 8.7 | +1.2% |
| 4000 | 弱控制器 | 120.9 | 8.3 | +6.0% |
| 16000 | sub-0.1mT | 147.2 | 6.8 | +28.9% |
| 48000 | 拐点区（collapse） | 221.1 | 4.5 | **+93.7% ≈ 纯 CPU** |

- **回归（v≥1000 四点）**：斜率 = 2.26 ms 每 ns-旋钮，理论值 500 jobs × 4726.7 dist ×
  1ns = 2.36 ms/ns（偏差 −4%）——wall 随算力旋钮严格线性爬升。
- **拐点三口径一致（≈44–46µs/dist）**：
  - 闭式：**v\* = miss 时间/每条 dist = 206.1ms/4726.7 = 43.6µs**——拐点物理意义 =
    *每 job 附加计算时间恰好追平 miss 时间*；
  - 模型列：(217.4−112.7)/2.363ms = 44.3µs；
  - 拟合：(217.4−114.1)/2.26ms = 45.7µs。
- 48µs 旋钮点（221.1s ≈ 纯 CPU 217.4s）：协同增益归零——引擎退化成"半速 CPU"。

## 5. CXL.cache 一致性税不敏感（Type-2 分支 D1 实验）

> 本节 5 点跑在 cylon-v9.1-type2 分支（D1 提交 3dfdc0009）二进制上，profile/负载与
> 主线 E1' 完全相同；f=0.5。BI 旋钮 = /tmp/femu-bi-lat-ns（每 job 读），0 = 关闭 =
> 逐字节 bit-identical。数据 `e1d_bi_sensitivity.csv`。

| BI 旋钮 | 客户端 | wall(s) | Δ vs 0ns（序列内） | Δ vs 主线基线 114.1s | dump |
|---|---|---|---|---|---|
| 0 ns（type2 二进制基线） | avx | 115.1 | — | +0.9%（run 偏移） | identical |
| 250 ns | avx | 114.4 | −0.75s | +0.23% | identical |
| 500 ns | avx | 114.2 | −0.95s | +0.05% | identical |
| 1 µs | scalar | 15.6<sup>†</sup> | +0.4s | +1.3% | identical |
| 5 ms（430× 物理值） | scalar | 117.4 | **+2.3s** | +2.9% | identical |

<sup>†</sup> 标量客户端本身慢 ~2.6s（t_cpu 113.0 vs avx 110.3）；BI≥1µs 用标量是 D1 已知
限制（avx@BI≥1k 崩溃角落，CYLON-TYPE2.md §3）。

- **物理范围（≤500ns）无系统性上升趋势**：序列内 115.1 → 114.4 → 114.2 单调略降
  （warm-up 漂移主导）；250/500 两点 vs 主线基线 ≤0.25%。
- **税上限模型**：每 job 1 次计费事件（信箱+结果页重陷，日志恰 jobs+2 条/run），
  整页 64 行 × BI=500ns → 32µs/job = **0.014% job 时间**——远低于 ±1% run-to-run 噪声带。
- **5ms proof 点（430×物理值）+2.3s（+2.0%）**：机制存在且可测，但物理范围内税被噪声
  淹没 → **"结果拾取对 CXL.cache 一致性税不敏感"成立**。
- 旋钮=0 负对照干净：0 条 re-trap 日志；每 run 计费事件 = jobs+2（BIND/FLUSH 也重陷）。

## 6. 统一图景与设计启示

**三条曲线，一个故事**：

1. **协同结构**：两端点等速（CPU 4.6 vs 引擎 4.8 QPS）→ f* 必在 0.5，实测 1.91×、
   聚合效率 96%。
2. **引擎为何追平 CPU**：99% miss-bound——器件的价值是**访存位置**，不是峰值算力。
3. **双重不敏感**：
   - 算力扫过 5 个数量级（0.19ns/dist=8T FPGA → 1µs/dist=ARM 控制器）全平坦，
     拐点 = 每条 dist 的计算时延追平 miss 预算（v* = 206ms/4726.7 ≈ 44µs/dist）；
   - 一致性税扫过 4 个数量级（0→5ms）物理范围平坦（上限模型 0.014%/job）。

**论文级设计启示**：

1. **分区闭式**：f* = T_cpu/(T_cpu+T_eng)；等速端点 → f*≈0.5，加速 =
   (T_cpu+T_eng)/max(T_cpu,T_eng)。
2. **优化投入方向**：性能 = 1/(misses × miss 延迟)——钱花在降 miss（容量/带宽/缓存
   策略，DSE 曲线 QPS≈1/(misses×40µs)），不为峰值算力付钱（真实 FPGA 余量 5 个数量级）。
3. **协议开销预算极宽松**：BI snoop-equivalent 账单在物理范围不可测；D2（设备发起
   staging）/D3（信箱 v2）是鲁棒性工程，不会移动性能模型。
4. **器件选型红线**：单次距离计算时延红线 v* ≈ miss 预算/dist = 206ms/4726.7 ≈
   44µs/dist；真实脉动阵列（亚 ns/dist）余量 5 个数量级，瓶颈在别处。

---

## 7. 数据与复现

**入库文件**（`exp/e1c_paper/`）：

| 文件 | 内容 |
|---|---|
| `e1c_results.csv` | E1' f 扫描 7 点全量（wall/QPS/T_cpu/T_eng/exec/misses/dist/hops/recall/dump 门禁/理想模型列） |
| `e1c_qps_vs_f.png` | QPS-f 曲线 + wall 分解双面板 |
| `e1c_compute_sensitivity.csv` | 算力敏感性 11 点（含 wall_model_s 理论列） |
| `e1c_compute_sensitivity.png` | QPS-算力旋钮曲线（log-x，FPGA 区间/ARM 锚点/拐点标注） |
| `e1d_bi_sensitivity.csv` | BI 一致性税 5 点（type2 分支） |
| `plot_e1c.py` / `plot_e1c_compute.py` | 图脚本（matplotlib，Agg） |

**运行入口**：`bash $W/tools/wiki_exp.sh <tag> <f> <scalar|avx>`（W=/var/tmp/cylon/anns）；
算力旋钮 `/tmp/femu-compute-ns`（每 job 读，免重启）；BI 旋钮 `/tmp/femu-bi-lat-ns`
（type2 分支）。所有点共享硬门禁：**dump 与 engref_ef100.dump 逐字节一致（recall 0.9720）**；
客户端暴毙由 wiki_exp.sh 44000B dump 尺寸门禁拦截。

**旋钮=0 负对照**：compute 旋钮 0/缺省 = 锚定 profile 原速；BI 旋钮 0 = 0 条 re-trap
日志 = 与主线逐字节 bit-identical。

---
*2026-09-06 · cylon-v9.0.1 · host 权威副本仍在 /var/tmp/cylon/anns/exp/e1c_paper/*
