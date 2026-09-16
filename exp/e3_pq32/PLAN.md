# E3 — 「 decent-recall PQ 配置 → CPU+swap baseline → 加速器对比 」战役计划

## Phase A 结果（2026-09-13 完成）
- Trainer/ref searcher/build_cyh2 全部参数化（-m / -M / <pq_m>）；PQ16 回归 md5 7a50ae34… 精确复现（参数化无损）。
- **PQ32×8 五臂曲线（ref searcher, cyh1_wiki.blob + PQ32 codes）**：
  recall@10 = 0.7197 / 0.8728 / 0.9155 / **0.9458** / 0.9689 @ ef=R = 100/512/1024/2048/4096
- **门 PASS：PQ32@2048 = 0.9458 ≥ 0.90**（PQ16 同点 0.6107）。
- 引擎 byte-gate 目标 = arm3 dump md5 **d8986bceca1755465b2119b422ed9b07**（ef=R=2048）。
- PQ32 codes md5 30abf67f…（672.5MB），codebook md5 16441e1e…（786KB）。
- PQ32 A1 blob 导出 38,518,058,728B + header 校验：旧 build_cyh2 "+40 残留" bug 复现
  （off_codes −8，与 PQ16 aisaq_a1.blob 同根）——文件体已证正确（blob[ocb+cb] == codes.u8
  row0 逐字节），就地补 header @80: 37,845,569,120→37,845,569,128，重验 5 行 OK。
  整 blob md5 存 /var/tmp/cylon/anns/tools/pq32_a1_blob.md5（传输门禁用）。


背景：E2（PQ16×8）门禁全过但 recall 封顶 0.611@R=2048（R-knee 粗化），端到端不如 A0。
你的指令：先造一个 recall 还不错的 PQ 配置，再量纯 CPU+swap baseline 的速度+精度，
最后对比 CXL SSD 加速器能否进一步提速。方案分三段：

## Phase A — 配置搜索（宿主侧，无 FEMU）
- Trainer 已参数化（`aisaq_train_pq.py <M>`），PQ32×8 训练+全量编码已后台启动（~50min）。
- ref searcher 已参数化（`-m`）+ blob 改 mmap（为 Phase B 的内存上限设计）。
- 回归门：参数化后 PQ16 ef=R=2016 臂 dump 必须逐字节复现 7a50ae34…（正在后台跑）。
- **判定门：PQ32 图游走 recall@10 @ ef=R=2048 ≥ 0.90**（ref searcher 5 臂扫 100/512/1024/2048/4096）。
  不过就训 PQ64×8 再扫（训练 ~45min + 扫 ~10min）。oracle-R ceiling 只作诊断，不做门。
- 已知映射：PQ16 oracle-R = 0.40/0.78/1.00 @ R=100/1000/10000；图游走 0.611@2048。
  PQ32 量化噪声减半 → 预期 oracle-R@2048 ≈ 0.93-0.97；图游走会比 oracle 低一截
  （PQ16 时低 0.17），**风险：PQ32 图游走@2048 可能仍 < 0.90 → 那 PQ64 兜底**。

## Phase B — CPU+swap baseline（宿主侧，无 FEMU）
- 工具：参数化 ref searcher（算法 ≡ 引擎，已证）+ mmap blob（37.84GB CYH1 图 blob 留在 sda HDD）。
- 容量限制机制：cgroup v2 memory.max（systemd-run --user -p MemoryMax=…）——页缓存驱逐 =
  "内存不够 → 盘上取"，即用户所说 swap 语义（索引是 mmap 文件页，真实部署同机制）。
- 档位：{40GB（无约束，blob+codes 全驻）, 24GB, 12GB} × ef=R=2048，各跑冷/热两遍。
- 产出：baseline QPS/lat + recall（与引擎同 walk ⇒ recall 由 (ef,R) 唯一决定，两臂同 recall）。
- 诚实注记：baseline 的盘 = sda HDD (~150MB/s)；引擎的"盘"= FEMU 仿真介质（DRAM 后端），
  引擎 48GB tier = devdax DRAM。**在 123GB 内存宿主上，无约束档 baseline 全驻内存后，
  引擎唯一可能赢的档位 = 容量受限档（12/24GB）**——这正是加速器叙事的主场
  （DRAM 受限部署 + 大容量设备侧 tier）。

## Phase C — 引擎臂（FEMU 侧）
- pnm.c 已去硬编码（5 处：pin 尺寸、BIND 门 {16,32,64}、c[64]、A1 步长、A2 rec 偏移），
  QEMU 重建绿。重启 FEMU 前不生效（当前运行中的 FEMU 还是旧二进制）。
- PQ32 A1 blob 重导出：38,518,058,728B（= cyh1 37,844,782,648 + 48 + 786,432 + 672,489,600）。
- Guest 盘 24GB 空闲 → 需删 aisaq_a2.blob（36.46GB，G3 门禁已过、矩阵已入库，宿主有备份）
  才放得下 38.52GB PQ32 blob。**将执行**（有异议请喊停）。
- 传 guest（SLIRP rsync，小时级）→ stage（dev-staging ~8.7min @74MB/s）→ BIND/byte-gate
  （引擎 dump == ref PQ32 ef=2048 臂 dump）→ 计时矩阵。

## 时间线（后台为主，无值守）
- A：训练 ~50min → 回归+5 臂扫描 ~15min → 门判定
- B：与 A 的训练并行不可（同 HDD 竞争），A 完成后 ~1.5-2h
- C：blob 导出 ~10min → rsync 38.5GB 数小时（无值守）→ stage+gate ~20min
- 每个阶段完成即报告数字，全链路 md5 门禁 + cgroup 实测
