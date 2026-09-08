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
| BI 税 × f 全网格（E1''） | **平坦（最大 +3% 非单调，噪声带内；17/18 格）** | §5.1 |

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

### 5.1 E1''：BI 税 × 协同全网格（2026-09-06，type2 分支）

> BI ∈ {250, 500, 1000}ns × f ∈ {0, .25, .5, .65, .75, 1}，**17/18 格**逐字节过门禁
> （f=0.65@1µs 客户端 3/3 崩溃，见下）。数据 `e1pp_matrix.csv`，图 `e1pp_matrix.png`；
> BI=0 参考列 = `e1c_results.csv`（D1 已证 knob=0 与主线 bit-identical）。

| f | BI=0（E1'） | BI=250 | BI=500 | BI=1000 | 行内最大偏差 |
|---|---|---|---|---|---|
| 0 | 208.3 | 208.3 | 204.1 | 208.3 | ±2% |
| 0.25 | 159.8 | 163.8† | 161.6 | 164.6 | +3% |
| **0.50** | **114.1** | **114.5** | **110.2** | **112.1** | ±2% |
| 0.65 | 142.5 | 142.8 | 142.7 | —‡ | — |
| 0.75 | 163.7 | 165.6 | 164.1 | 168.5 | +3% |
| 1.0 | 217.4 | 217.4 | 217.4 | 227.3§ | — |

† scalar 补格（avx 4/4 崩）；‡ 3/3 崩，格子关死；§ scalar 纯 CPU 锚点（227.273 vs
E1' scalar 227.3 逐位复现）。

- **BI 维全网格平坦**：每行跨 BI 列最大偏差 +3%（f=0.25）且**非单调**（250 列 163.8 >
  500 列 161.6）→ 噪声带，无系统趋势。税上界模型（1µs × 4.3 jobs/query ≈ 275µs vs
  引擎 208-230ms ≈ 0.12%）在**整个 f 空间**成立，不再只是 f=0.5 单点。
- **跨月逐位复现**：f=1 锚点 avx 217.391（E1' 217.4）/ scalar 227.273（E1' 227.3）——
  宿主重启 + 换内核（rev -11）后 bit 级复现，回归参考价值高。
- **一图三义**：协同有大区别（f*=0.5 → 1.91×）；引擎算力 5 个数量级不敏感（§4）；
  Type-2 一致性税不敏感（本节）。性能全部来自**访存位置**，优化方向 = 降 misses。
- **两族客户端崩溃（模拟器 bug，D3 修复对象；数据无恙——存活格 dump 全部逐字节）**：
  1. **avx #UD 族**（ip 0x110b，取指拿错页字节）：f=0.25 毒点——b250 4/4 死、b500 2/3
     （v2 过，重启换 ASLR 基址复活）→ 非确定性竞态；
  2. **scalar GPF 族**（ip 0x25d0，E1'' 新发现）：knob=1k × 高 f（CPU 侧大活跃）——
     f050 1/2 死、f065 3/3 死（三个 ASLR 基址同一偏移）→ 该操作点确定性。
  规避法：毒格换客户端补跑（BI 计费与客户端 ISA 无关，D1 已证）；f065@1k 由
  f050/f075 行夹逼，曲线约束完整。

### 5.2 E-S：设备发起 staging（2026-09-06，type2 分支 D2）

> staging 方式对比 {first-touch（guest 单核逐页 trap→FTL）, 设备发起
> `PNM_OP_STAGE`（引擎从介质 bulk 重填缓存）}，wiki 36GB / 9.2M 页 / 512MB devdax
> 缓存（→ ~9.1M 次内部驱逐+干净回写发生在每次设备填充中）。账单旋钮
> `/tmp/femu-stage-bps`：缺省 2e9、0=不计费。数据 `es_results.csv`，图
> `es_staging.png`；门禁 = dump 与 engref 逐字节一致，7/7 有效点全过。

| 点 | 方式 | bps | staging wall | search wall | recall | 门禁 |
|---|---|---|---|---|---|---|
| es_fresh | ft 冷（FTL map 冷） | — | **2003.3 s** | 112.8 | 0.9720 | PASS |
| es_ft_restage | ft 暖（FTL map 暖） | — | 520.9 s | 113.5 | 0.9720 | PASS |
| es_slow | dev | 0.5e9 | 75.7 s | 112.6 | 0.9720 | PASS |
| es_t2 | dev | 14e9 | 25.3 s | 111.5 | 0.9720 | PASS |
| es_t0 | dev 下限 | 0（不计费） | **21.3 s** | 110.8 | 0.9720 | PASS |
| es_t1 | dev 缺省 | 2e9 | **21.1 s** | 117.2 | 0.9720 | PASS |

- **95× / 24.7× 提速**：dev 缺省（21.1s，下限主导——2GB/s 建模带宽≈真实 bulk 率
  ≈1.7GB/s）vs ft 冷 2003.3s（9.2M 页 × 208µs NAND program 账单主导，建模）/ ft 暖
  520.9s（read 账单）。0.5e9 点 75.7s = 计费显形（72s 目标 + 交错）；14e9 点 25.3s
  仍下限带内。
- **搜索侧不变性**：全点 search wall 110.8–117.2s（±2.8% 带）、recall 恒 0.9720、
  dump 全逐字节=engref → **staging 方法在测量区间之外**，这是"方式可互换"的许可证；
  引擎 miss 数同带内（4290–4301/q）。
- **方法论边界**：E-S 里 Type-3 的 staging 时间是被测对象，必须 ft 忠实跑；Type-2
  叙事（本节）用 dev。每 boot 首跑必须 ft（建 maptbl，覆盖 0 → 客户端自动回退），
  之后全 dev。
- **诚实性**：两条臂都是建模的——ft 臂 = 逐页 trap + NAND 208µs program/read 账单；
  dev 臂 = max(真实填充, bytes/bps) 双精度截止自旋。RFO 路线（设备经 CXL.cache 拉
  宿主 DRAM）deferred，FTL-prefetch 是其建模替身。
- es_t1 第 1 次尝试客户端死（avx GPF 族，§5.1 已知竞态，非 D2 引入；引擎 jobs
  81–86 正常完成）→ 文档化重试协议，重试 7/7 过。

### 5.3 E-M：信箱 v2 + 门铃——等待方式 × 毒格死亡率（2026-09-07，type2 分支 D3）

> 协议 v2（u64 代际状态字，客户端独占 gen，消 ABA）+ 等待方式三臂
> {poll = v1 紧自旋, sleep = 2µs+50µs, doorbell = poll(/dev/cylon-db) 阻塞等
> MSI-X}，在两个崩溃族毒格各 3 重放（dev staging，BI 旋钮按格设定）。数据
> `em_results.csv`，图 `em_battery.png`；门禁 = dump 与 engref 逐字节（硬门）。
> 附门禁：G1 legacy 112.485s ✓、G5 doorbell 114.239s ✓（IRQ 计数 515 证投递）、
> G4 协议时序门 112.869s ✓（§下）、CONTROL v1@v2 引擎 146.265s ✓（fresh boot）。

**主结果表**：

| 毒格（族） | poll | sleep | doorbell | v1 历史对照 |
|---|---|---|---|---|
| scalar f=0.65 @ b1000（poll-storm 族） | **3/3 活** | **3/3 活** | **3/3 活** | v1 紧自旋 3/3 死；v1 sleep 3/3 活 |
| avx f=0.25 @ b250（flip/flush 族） | 0/3 | 0/3 | 0/3 | v1 紧自旋 4/4 死；v1 sleep 4/4 死 |

- **scalar 族（§8.10.4-2）被 v2 世界根治，归因 = 引擎侧（CONTROL 臂定案）**：
  9/9 全活（dump 全逐字节），wall 带 143.7–147.4s（±1.3%，三臂同带；参考
  e1pp_b250_f065=142.8s）——包括**紧自旋 poll 臂**（v1 同臂 3/3 死）。
  **CONTROL 臂 `ctrl_v1_f065b1000` = v1 协议客户端 + 紧自旋，对同一 v2 引擎跑
  同一毒格（fresh boot）→ 活，146.265s 带内，dump 逐字节** → 客户端协议改动
  不是必要因子，**治愈因子在引擎侧**（v2 引擎的 u64 {gen,DONE} 单 store 发布 +
  gen 拾取检查，取代 v1 引擎对 status 字的双写者竞争模式）。该族顺带填补了
  E1'' 矩阵唯一死格 b1000_f065（144.8s 均值，落 BI 平坦带）。
- **avx 族（§8.10.4-1）确认与等待方式无关**：10/10 死（poll/sleep/doorbell ×3 +
  G4-avx 尝试），全部 #UD（根因 = KVM 模拟器不解码 VEX，§5.4 F4 定案）、**launch
  后 ~1.3s**（甚至到不了 search——BI churn 已在跑，任何 avx 指令窗口都是根因路径）。
  **客户端侧缓解空间对该族已穷尽**（spin/sleep/block 全灭）→ 出路 = 引擎/内核侧
  根治，**已由 §5.4 F5 "bill & re-execute" 兑现**（F6 电池 6/6；门铃臂复跑见
  item 4/5）。门铃对该族无罪（Phase 0 预判命中）。
- **门铃时序无害**：doorbell 臂 wall 与 poll/sleep 同带（job ~200ms，µs 级唤醒
  不可见——门铃的价值在去 CPU 燃烧与风暴，不在 job 时延）；G5 全程 IRQ 515 次。
- **G4 时序门**：b250_f050 带 = 114.451±3% = [111.0, 117.9]。avx 尝试死于族
  （=上面第 10 例）；**G4S = `g4_b250_f050_scalar` 112.869s ✓ PASS**（dump 逐字节；
  注：该 run BI 旋钮被 phase-3 driver 清掉 = BI 0，BI∈{0,250} 差 <0.05% wall 在带内
  无差别；构建不同同理由 E-S §5.2 跨构建同带背书）。
- **诚实边界**：v1 毒格数据是历史 boot 的（3/3、4/4），与今日 v2 电池非同
  session；n=3/臂。CONTROL 臂（fresh boot）把归因定为引擎侧，但 fresh-slot/boot
  与 v1 历史死亡时的长会话 boot 构成第二个 delta（缓冲证据：v2 电池 9/9 活发生在
  长会话 late-boot，boot 新鲜度不太可能是保护因子；v1 客户端在脏槽上会被 v2 引擎
  永拒——gen 半字继承残留——见 USAGE §8.11 协议边角，故 CONTROL 必须 fresh boot）。
  ≥200 run 门铃零 GPF 压测未跑（单臂 ~10min 量级 → 200 run ≈ 33h，本战役不覆盖）；
  现有证据 = 毒格 3/3 + CONTROL + G5 门禁 + Phase 0 历史。

### 5.4 avx #UD 族根因定案与修复（2026-09-07，type2 分支 D3-F：F1–F5）

> 取证（F2/F3）→ 根因（F4）→ 修复（F5）。取证对象 = avx 毒格 f=0.25@b250 的
> 确定性 #UD 死亡；工具 = `ulimit -c unlimited` + `core_pattern` 改真实文件
> （apport 管道会吞 core）、gdb 剖 core、内核源码链。**结论：与"翻页竞态"无关，
> 是 KVM 内嵌 x86 模拟器不会解码 VEX/AVX 指令**；客户端/引擎/协议全链无责。

**F3 现场（core.4389，md5 b54e7de7dbba1198e6f81d2189791780）**：`#0 mb_submit+131
（本构建偏移 0x2983）`，故障指令 `vmovq %xmm0,(%rax)`（真 4B VEX 存储，非错页
取指——旧"取指拿错页字节"论作废），rax = 窗口末页 = **信箱页**。dmesg 双尸
（pid 4389/4716）同偏移 0x2983 = 确定性。判别器 A/B/C：avx 打残留 trap 信箱 =
必死（故障指令不退休，leaf 永停 trap）；scalar 同点 = 活且"自愈"（标量可解码 →
走老模拟器路径 → DUAL 检查（x86.c:7651）→ MMIO fragment → QEMU 计费 + 翻直，
"清了毒"）；avx 后行 = 活，158.871s 带内（E1' 参考带 110.8–159.8s），dump
md5 ec3059fbb6561f67fb2b2402603fbf3c = engref 逐字节。**BI 旋钮缺席 = BI off**
（pnm.c:921 `bi_ns = 0` 缺省），故本轮判别器全程 BI=0，毒信箱 = 前-session
遗留态（battery 死 run 与 CONTROL 末次 retrap 竞态是再生机制，细节不可复原，
但不影响修复有效性）。

**F4 内核证据链**：DUAL 槽缺页 → tdp_mmu `make_mmio_spte` 装陷阱叶
（tdp_mmu.c:990）→ RET_PF_EMULATE → `x86_emulate_instruction` → 解码器所有 VEX
处理被注释（emulate.c:1204/4839/5011）→ `emulate_ud`（:608）→ 注入 #UD。即：
**任何 VEX 指令触 trap 叶（信箱 retrap 后的首触，或数据页驱逐后首触）都在
DUAL 检查之前死于解码**——两处死亡位点（启动期 mb_submit VEX 填充、搜索期
pnm_dist 窗口 VEX 加载）同一机理。scalar 永不死：标量指令解码成功 → 走老路径
计费。avx 族的"非确定性"（E1'' 11/12 绿、E-M 10/10 死）= **毒药再生的竞态**：
每个完成 run 的末次 retrap 落在客户端末次拾取之后（µs 窗口）→ 信箱带毒收场；
下个 avx 客户端首触即死；scalar 客户端首触即"自愈"。fresh boot 的 bring-up
必过 = init 时 tail 区已 pin 直（无毒可染）。

**F5 修复（"bill & re-execute"，机制零改动）**：内核 `kvm_mmu_page_fault` 在
DUAL 槽 RET_PF_EMULATE 处不进模拟器，直接挂新 UAPI exit `KVM_EXIT_CYLON_DER
（40）`，payload = {gpa, is_write}（**不解码**）；QEMU 端 kvm-all.c 接 exit →
FEMU 计费走与老 trap 路径完全同路（tail 页：pin+BI 计费；数据页：
`wait_for_buf_update(..., data_ptr=NULL)`——ftl_thread.c:140 只跳过 memcpy，
cache 填充+EPTE 翻直照常），翻直后 RIP 不变重进入 guest，指令**原生重执行**
（ISA 无关，VEX 亦然）。计费等价性：scalar 老路径 = 每 (re)trap 一次计费
（首个访问计费+翻直），F5 = 每 violation 一次计费+翻直——**每 (re)trap 一次，
等价**；D1 的"真实翻页"叙事原封不动。兜底：同 gpa 重试 >3 次自动回退老模拟器
路径（防活锁）；plain/skip_ftl 模式 handler 拒绝 → 同兜底。**部署 = L0 内核
6.4.6-cylon rev -13（deb 构建）+ QEMU 重建**；L2 guest 内核/客户端/引擎全零改动。

**F6 验收（2026-09-07 晚，rev -13 + UAF-fix QEMU md5 3701e149，f6b 链）**：
1. **门禁 G1–G4 全过**（18:17–19:34）：G1 v1-first（fresh boot、FTL 重建，
   byte-identical）→ G2 poll → G3 sleep → G4 E1'' 关键格 b250 f0.5 avx——
   **G4 = 前一轮 QEMU GPF 的崩溃格**，修复后一次通过；全程无 "REFUSED
   under-lock" 打印（UAF 修复的锁内拒绝路径零触发）。
2. **QEMU GPF 根因（同日破案，cache UAF 竞态）**：引擎线程（pnm.c:980
   "cylon-pnm"）与 FTL 线程（ftl_thread.c cxl_req 消费者）共享 cache g_tree；
   `cache_lookup`（cache.c:284）无锁 `g_tree_lookup` 撞 insert 的锁内
   evict+free → 命中路径 entry mid-use 被释放复用，堆垃圾充 lpn → 填充
   memcpy 到非规范地址 → host #GP（PIE 符号化勘误后定位 memcpy←cylon_cache_insert，
   见 caveats #56）。修复 = `cache_lookup` 加 `c->lock` + insert 锁内 lpn 边界
   复查（拒绝 → 槽位归还 + 客户端 re-trap → retry-cap 兜底）。ftl_thread.c 零改动。
3. **E-M 电池 avx 毒格 poll/sleep ×3 = 6/6 活，dump 全部 byte-identical**
   （poll 165.4/161.6/160.9s；sleep 161.8/161.9/161.8s）。F5 的 avx 治愈成立。
4. **doorbell 臂 = 投递死（2026-09-07 深夜破案：陈旧 .ko 二进制——修复写进源码但从未重编）**：
   F6 两轮探针 IRQ 0→0（含 pin CPU0+停 irqbalance）当时误判为"SIGNAL_MSI 路径本身断"。
   调试构建（6b3c54c1：`kvm_irqchip_send_msi` 打印 SIGNAL_MSI addr/data/ret + doorbell.c
   打印 notify 现场+MSI-X 表值）取证：**notify 触发 + 表值完美（tbl=0xfee01004/0x23
   unmasked used=1）但零 SIGNAL_MSI 打印** → 断点在 QEMU dispatch 内（throwaway QEMU
   `info mtree -f` 确认 kvm-apic-msi @0xfee00000 prio 4096 正常、bus_master_as 走
   bus_master_enable_region 别名）；`lspci` **BusMaster-** 定罪 → `nm -u` 陈旧 .ko
   **缺 pci_set_master 符号**（.ko 18:17 构建，早于 18:54 的 pci_set_master 修复编辑——
   修复写进源码但**从未重编**，历次 boot 一直在 insmod 无修复的旧模块）→ 无 master →
   QEMU `bus_master_enable_region` 别名禁用 → 门铃 bus_master_as 扁平视图空 → MSI 写落
   unassigned 空间**静默消失**。重编 .ko → BusMaster+ → **[CYLON-DBG] ret=1（内核注入
   1 MSI）+ IRQ 0→4**，投递复活。**dump 门禁对投递死不敏感**（客户端 ~10s/engine-job
   门铃等待超时兜底走完），wall<180s=活 / >5000s=死仍是唯一判别器。
5. **门铃电池臂 ×3 复跑 + G5 复活（2026-09-08 凌晨，fixed .ko）= 4/4 全过 byte-identical**：
   G5 门禁 117.069s（BI=0；替换死节奏假数据 5004.536s，与 9/6 原门禁 114.239s 同带）；
   avx 毒格门铃臂 **165.503 / 163.694 / 161.977s 3/3 活**，与 poll（165.4/161.6/160.9）、
   sleep（161.8/161.9/161.8）完全同带 → **门铃时序无害在修复后世界复确认**。csv/png 已
   重组（21 行全 PASS，18/18 电池 + 3 门禁）。插叙：doorbell_2 首跑"死亡" = wiki_exp.sh
   的 ssh 会话断裂（客户端随 sudo 链 SIGHUP 真死，0 字节 dump 假 CLIENT DIED 判据碰巧
   正确），非客户端崩溃；复跑 163.694s 一遍过。**遗留待办（低优先级）**：scalar 毒格
   fixed-binary 复跑（9/6 的 3/3 活数据已是 live 投递，非必跑）；移除调试打印
   （kvm-all.c SIGNAL_MSI 打印 + doorbell.c notify 打印，cap 5 次后静默，不影响数据）。

## 6. 统一图景与设计启示
**三条曲线，一个故事**：

1. **协同结构**：两端点等速（CPU 4.6 vs 引擎 4.8 QPS）→ f* 必在 0.5，实测 1.91×、
   聚合效率 96%。
2. **引擎为何追平 CPU**：99% miss-bound——器件的价值是**访存位置**，不是峰值算力。
3. **双重不敏感**：
   - 算力扫过 5 个数量级（0.19ns/dist=8T FPGA → 1µs/dist=ARM 控制器）全平坦，
     拐点 = 每条 dist 的计算时延追平 miss 预算（v* = 206ms/4726.7 ≈ 44µs/dist）；
   - 一致性税扫过 4 个数量级（0→5ms）且 × 全 f 网格（E1'' 18 格）物理范围平坦
     （上限模型 0.014%/job）。

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
| `e1pp_matrix.csv` | **E1'' 全矩阵 18 格**（BI {250,500,1000} × f 网格；f065@1k 空格含死因注记） |
| `e1pp_matrix.png` | BI 税 × f 全网格曲线（三 BI 列近乎重叠） |
| `e1pp_assemble.py` / `e1pp_plot.py` | 矩阵汇编（直读 dump 逐字节校验）与绘图脚本 |
| `es_results.csv` | **E-S staging 对比 7 有效点 + 1 崩溃记录**（type2 分支 D2；bps/staging wall/search wall/recall/门禁） |
| `es_staging.png` | staging 方法对比（log 柱高）+ 搜索侧不变性双面板 |
| `plot_es.py` | E-S 图脚本（matplotlib，Agg） |
| `plot_e1c.py` / `plot_e1c_compute.py` | 图脚本（matplotlib，Agg） |

**运行入口**：`bash $W/tools/wiki_exp.sh <tag> <f> <scalar|avx>`（W=/var/tmp/cylon/anns）；
算力旋钮 `/tmp/femu-compute-ns`（每 job 读，免重启）；BI 旋钮 `/tmp/femu-bi-lat-ns`
（type2 分支）。所有点共享硬门禁：**dump 与 engref_ef100.dump 逐字节一致（recall 0.9720）**；
客户端暴毙由 wiki_exp.sh 44000B dump 尺寸门禁拦截。

**旋钮=0 负对照**：compute 旋钮 0/缺省 = 锚定 profile 原速；BI 旋钮 0 = 0 条 re-trap
日志 = 与主线逐字节 bit-identical。

---
*2026-09-06 · E1'/算力/BI-5 点主线 = cylon-v9.0.1；E1'' 全矩阵 = cylon-v9.1-type2（D1
二进制）· host 权威副本仍在 /var/tmp/cylon/anns/exp/e1c_paper/*
