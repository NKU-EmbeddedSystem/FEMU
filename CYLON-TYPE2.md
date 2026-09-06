# CYLON-TYPE2 路线图 — CXL Type-2（设备一致内存 + CXL.cache）扩展

状态：**规划文档**（本分支 cylon-v9.1-type2，自 cylon-v9.0.1 @2e32446c2 分出，尚无代码）。
主实验线仍在 cylon-v9.0.1（Phase C：21M×768d wiki_dpr_e5，E1' 已收官）。
引用背景见 CYLON-USAGE.md §8.9.8（真实器件锚定）与 §8.10（Phase C）。

## 0. 一句话目标

把模拟器从"CXL Type-3 内存扩展 + 宿主进程内模拟引擎"升级为"CXL **Type-2**：设备一致
内存（HDM-DB）+ CXL.cache（BI snoop）+ Device Atomics"，并量化两件事：
**一致性税**（CPU↔设备交接路径的 snoop 成本）与**设备发起 staging**（替代 33 分钟
guest first-touch）的收益。

## 1. 现状盘点：我们在哪些地方"作弊"

| CXL 语义 | 现实现 | 作弊程度 |
|---|---|---|
| CXL.mem（HDM 内存扩展） | 48GB 窗口，guest devdax | 忠实 |
| 一致性 | 设备内存 = 宿主 DRAM 单一物理副本，CPU 硬件白送 | **全免** |
| 近存计算引擎 | FEMU 进程内宿主线程，绕 EPT 直读同一 DRAM；延迟=账单模型 | 计算免费、延迟靠账单 |
| CPU↔设备握手 | 窗口内软件信箱（两边轮询裸字 doorbell） | 无协议成本 |
| 行粒度一致性 | DER 页粒度 EPT 翻转（hypervisor 私有） | 非 CXL 语义 |
| 设备算力 | FP16 数 T 脉动阵列画像 → comp_dly=0（§8.9.8） | 忠实（可忽略） |

## 2. Type-2 语义差集

Type-2 = 设备有自己的内存（**HDM-DB**：Device-Coherent with Back-Invalidate；
或 HDM-H：Host-Coherent，语义同 Type-3 region）+ 作为 **CXL.cache 代理**参与系统
一致性：

- 设备写自身内存时，必须能把 CPU 侧缓存行打掉（**BI snoop**）；设备缓存宿主内存行
  时，响应宿主的 BI 脏驱逐（DZH/DROPD 类事务；**动笔写论文前逐条对 CXL 2.0+ spec
  核对事务名**）。
- 设备可主动 RFO 宿主内存（CXL.cache 请求流）→ 设备发起取数成为可能。
- **Device Atomics**：CAS/fetch-add 在设备上执行。
- DER 与设备类型正交：DER（EPT 页翻转）保留为近存寻址的性能模拟基底，行粒度一致性
  语义改由模拟出的 BI 承担。

## 3. D1 — BI 一致性账单（最小可发论文增量）

**动机**：collab 拾取路径（客户端从信箱读引擎结果）目前是纯 DRAM 读、零一致性成本；
真实 Type-2 每次引擎写→CPU 读交接付 snoop 往返（~200-400ns 量级 + 行粒度序列化）。

**实现**（FEMU 侧，**已实现 2026-09-05**）：
- pnm.c job 完成路径：信箱页 + 结果页 re-trap 为 trap 态（`der_kvm_epte_retrap_control`）；客户端下一次拾取读触发 EPT violation 进 FEMU，trap 路径按 cacheline 数补收 BI 延迟后自锁直映射（`pin_tail_page`）。活调旋钮 `/tmp/femu-bi-lat-ns`（pnm 线程每 job 读一次，0 = off = 与 E1' 行为逐比特一致，knob 语义同 comp_dly）。
- re-trap 伴随 `der_kvm_flush_tlbs`（KVM_DER_FLUSH_TLB ioctl）——否则 vCPU TLB 缓存直映射、客户端永远不 trap，静默漏计费；**FEMU_DER_FLUSH=0 时 BI 账单一并失效**。
- re-trap + flush 每 job 一次（µs 级引擎侧开销，属模拟器机制成本，不计入被建模账单）。
- 负对照是 bi=0 这条线本身。**修正**：f=0（纯引擎）不是零账单负控——客户端在所有模式下都驱动信箱（提交+拾取），f=0 同样付每查询 2 次 trap 计费；正确的"无账单"对照是 knob=0。

**实验（f=0.5 wiki 21M 列，2026-09-05 验收）**：BI ∈ {0, 250, 500} avx + {1000, 5e6}
scalar，dump 全部逐字节=engref；wall 115.1 / 114.4 / 114.2 / 115.6 / 117.4 s —— 物理范围
（CXL.cache snoop 200-400ns）内账单 <0.05% wall（淹没在 ±2s 噪声带），结论成立 =
**"结果拾取对一致性税不敏感"**；knob=5e6（430× 物理）proof 点 +1.8s 浮出机制，每 run
恰好 jobs+2 次计费（BIND/FLUSH 各一）。负对照 knob=0 在 FEMU 日志层亦 0 条 re-trap。
**已知限制（2026-09-06 E1'' 后修订）**：两族客户端崩溃，同属"翻译过期竞态 × 客户端
活跃窗口访问"（D3 信箱 v2 结构性修复对象），引擎与数据路径无责（存活点 dump 全部逐字节
=engref）：
1. **avx #UD 族（0x110b，取指拿错页字节）**：f=0.25 毒点——b250_f025 4/4 死、b500_f025
   2/3 死 + v2 过（重启换 ASLR 复活）→ 非确定性；其余 11 个非-f025 avx 点全绿。
2. **scalar GPF 族（ip 偏移 0x25d0，新发现）**：knob=1000 × 高 f（CPU 侧大活跃）——
   f050 1/2 死、f065 2/2 死（基址不同 = ASLR 无关）。
规避法：毒格改用另一客户端补跑（BI 计费与客户端 ISA 无关，D1 已证）；f065@1k 三试
3/3 未复现（三个 ASLR 基址同一 ip 偏移 = 该操作点确定性），矩阵 17/18 格 + 该格上下由
f050/f075 夹逼。
**E1'' 全矩阵已完成（2026-09-06）**：BI ∈ {250,500,1000} × f ∈ {0,.25,.5,.65,.75,1}，
17/18 格逐字节过门禁（f065@1k 空格如上）；**BI 维全网格平坦（最大偏差 +3% 非单调，
噪声带内）**，与 D1 上界模型一致（1µs×4.3 jobs ≈ 275µs/query vs 引擎 208-230ms ≈ 0.1%）。
f=1 锚点跨月逐位复现（avx 217.391 vs E1' 217.4；scalar 227.273 vs 227.3）。数据
`exp/e1c_paper/e1pp_matrix.csv` + 图 + 汇编/绘图脚本；BI=0 参考列 = e1c_results.csv
（bit-identical 等价已证）。

**验收**：所有点 dump 与 engref 逐字节一致（BI 只改时序不改结果）；新增
`e1c_bi_sweep` CSV/PNG 进 e1c_paper/。

## 4. D2 — 设备发起 staging（CXL.cache RFO 拉取）

**动机**：现状 staging = guest CPU first-touch 38GB（~33 分钟，Phase C 最大部署痛点，
CYLON-USAGE.md §8.10.3）。Type-2 设备可主动 RFO 宿主内存：设备自己把索引从 guest
DRAM 拉进设备内存，速率由设备侧引擎决定，与 guest 单核缺页率解耦。

**实现**：
- 新 job 类型 `JOB_STAGE`（信箱 v1 兼容载荷）：{blob 在窗口外的 guest 物理地址表,
  目标设备区间}；引擎按大块（2MB）搬运并按 PCIe4-x8 上行带宽账单（~14GB/s 上限，
  32GB 向量段 ≈ 2.3 分钟上限）。
- 客户端只交出物理地址表（/proc/self/pagemap 或预留 memfd），不再逐页 first-touch。
- 保留 first-touch 路径作 Type-3 对照（同一份数据两种 staging 的对比图）。

**实验**：staging 时间对比（first-touch 33min vs 设备拉取上限 ~2.3min+FLUSH 成本）；
staging 后跑 f=0.5 一点验收逐字节一致。

**风险**：从 guest 用户态拿可靠物理地址表是脏活；备选 = 走既有 window 直写路径
（不 RFO guest DRAM，而是设备从"虚拟 NAND"读——即 staging 语义改成 FTL 后台预取，
绕开 pagemap）。

## 5. D3 — 信箱 v2：Device Atomics + MSI-X doorbell

**动机**：现信箱 = 两边轮询裸字（已知 collab GPF 竞态的最大嫌疑，§8.10.4：客户端或
FEMU 随机中签）。Type-2 提供 Device Atomics + 中断式完成通知，语义上根除轮询竞争。

**实现**：
- job 插槽状态机改原子 CAS：客户端 `fetch-add` 领号、设备 `fetch-add` 完成计数
  （FEMU 内模拟原子语义 + 账单延迟 `/tmp/femu-atomic-ns`）。
- 完成通知：设备对 collab 客户端发 MSI-X（QEMU MSI-X 基建现成；guest 侧 vfio？否——
  走 ivshmem/门箱中断通道与现有 DER 窗口解耦）。轮询保留为退化路径（开关回退）。
- **预期副产品**：单写者-单读者槽位 + 原子发布后，重跑 1000+ run 压测观察 GPF 消失。

**验收**：collab 压测（≥200 run）零 GPF；E1'' 关键点数字与 v1 信箱一致（±噪声）。

## 6. D4 — DVSEC 外观件（零性能影响，合规性）

- 设备模型从 cxl-type3 fork：加 **CXL.cache DVSEC**、**Device-Coherent Memory
  (HDM-DB) DVSEC**，CFMWS 把 region 标 device-coherent。
- guest 内核基本无感（BI 参与在硬件层）；devdax 路径不变。
- 只为"设备在 CXL 枚举层面像 Type-2"的论文可信度；可最后做或声明建模范围即可。

## 7. 实验矩阵与验收总门禁

| 实验 | 变量 | 交付 |
|---|---|---|
| E1''-BI | BI ∈ {0,250,500,1000} × E1' 7 点 | ✅ 2026-09-06：17/18 格，BI 维全网格平坦（<±3%），`exp/e1c_paper/e1pp_matrix.csv` |
| E-S | staging 方式 {first-touch, 设备 RFO} | staging 时间对比图 |
| E-M | 信箱 v1 轮询 vs v2 原子+MSI-X | GPF 率 + 拾取延迟 |
| 全局 | 所有结果类改动 | dump 与 engref 逐字节一致（只改时序的硬门禁） |

## 8. 非目标

- 真实行级 CXL.cache 事务模拟（消息级 MESI 状态机）——只做延迟/流量账单。
- HDM-H/DB 在 guest 内核里的差异化处理（软件透明）。
- 多设备/ fabric、P2P、动态容量（CDAT/DCD）。

## 9. 现实器件对照（论文 related work 锚点）

- Samsung CMM-D = Type-3（我们的现状语义）；CXL-PNM 原型（AXDIMM 类）多宣传为
  Type-2；我们的现状 = "Type-3 + 控制器内算力"，D1-D4 后 = 语义完整的 Type-2 近存
  设备模型。论文需明确声明此建模边界。

## 10. 建议切分与依赖

D1（~1 天，独立）→ D2（~2-3 天，独立，效果最直观）→ D3（~2 天，建议在 D1 数据
采完后动，避免中途改协议破坏可比性）→ D4（~1 天，可不做）。
每步在 cylon-v9.1-type2 上独立 commit；实验产物沿用 e1c_paper/ 命名法
（e2t2_*.csv/png）。
