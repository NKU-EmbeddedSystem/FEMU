# E2 — AiSAQ 复现：PQ16×8 + 全精度重排进 AASim 引擎

数据集：wiki_dpr_e5 21,015,300×768d fp16；1000 NQ 查询，k=10。
布局：A1（codes 追加在图后）与 A2（codes 内联节点记录）双臂。
门禁方法：dump 44B/查询（m + 10×u32 id）×1000；引擎 -F 0 全引擎臂；计费旋钮默认 0。

## 门禁状态

| 门 | 内容 | 目标 md5 | 结果 |
|---|---|---|---|
| G1 | A0 CYH1 回归（ef=100, -F 0） | ec3059fbb6561f67fb2b2402603fbf3c | **PASS** |
| G2 | A1 PQ byte-gate（ef=R=2048, -F 0） | 7a50ae34e2e3cc3cb1c2abd907d61460 | **PASS**（v4, 2026-09-13） |
| G3 | A2 自洽门：A2 dump == A1 dump == ref | 同 G2 | **PASS** |

G1 数字：QPS 4.2，p50 234.7ms，recall@10 0.9720，dist/q 4706.4，hops/q 114.4，
misses/q 4299.5。G2 数字：QPS 3.8，p50 266.0ms，recall@10 0.6107（ref 0.6106），
dist/q 92,393.2，hops/q 2065.2，exec 264.4ms/q，misses 4547.8/q，rerank 2048.0/q，
code-pages 0.0/q（A1 码区 pin 零缺失），vec-pages 2623.7/q。

## G2 首跑失败定案（2026-09-13）

症状：引擎 dump 与 ref 臂 0/930 行一致，recall 0.0516 vs ref 0.6106；rerank 计数
2048.0/q、code-pages 0.0/q（pin 生效）却 recall 崩塌。

根因（pnm.c，两处）：
1. **每查询 LUT 构建循环缺失**：`st->lut` 只有分配（BIND）与读取（pnm_adc_dist），
   从无写入 → LUT 恒全零 → 所有候选 ADC 距离恒 0 → res 堆按游走序塞满，ADC 路由
   退化为游走序采样。遥测全部吻合（hops ~2060、dist ~91K、rerank 2048、pin 零缺失
   ——管线在跑，只是排序信息全无）。
2. **上层贪心下降用全精度 pnm_dist**，ref 搜索器用 adc_dist —— 字节门禁要求游走
   逐位一致，降到 level-0 的入口点都不同。

修复：查询装载后补 LUT 构建（LUT[s][c] = Σ_{i<ds}(q[s·ds+i] − cb[(s·256+c)·ds+i])²，
ds=dim/pq_m=48，fp32，算术与 aisaq_ref_search.c 逐行同构）；下降两处距离换
pnm_search_dist（非 PQ 回落 pnm_dist，A0 路径零改动，G1 md5 保持）。

## R knee 粗化（须向用户提门限改案）

ADC rank 粗（根因 64bit/768d = 48d 子空间）：
- brute oracle-R：recall@10 = 0.40 / 0.78 / 1.00 @ R = 100 / 1000 / 10000
- adc-graph：0.280 / 0.454 / 0.531 / 0.611 / 0.698 @ ef=R = 100/512/1024/2048/4096

方案 v1.0 的 0.95 门限外推需 ef≈30-50K，引擎 ef 上限 4096，不可达。
提案：诚实曲线入库 + 操作点 2048/4096；0.95 改为"可达目标重协商"。

## R 封顶效应（ef=4096 首跑发现，2026-09-13）

blob header rerank_R=2048 在 ef=4096 时仍生效：res 堆 4096 个 ADC 候选，rerank 只取
top-2048 → recall 0.6090 ≈ ef=2048 的 0.6107（还略低——rerank 集合不同），远低于
ref（R=ef=4096）的 0.698。**knee 之上 recall 的绑定量是 R 而非 ef**。ef=4096 门禁臂
须宿主旋钮 /tmp/femu-rerank-R=4096 覆盖。该"封顶臂"本身保留为矩阵数据点
（header-R 语义验证）。

## 计费旋钮扫描（A1, ef=2048, 2026-09-13）

基线 (0,0) exec 264.37ms/q；dump 全格 md5 恒 7a50ae34（计费不扰动数据路径）。
- adc-ns=50 → exec 269.22ms（Δ+4.85ms；模型 92,393 dist × 50ns = 4.62ms ✓ 精确）
- adc-ns=200 → exec 281.75ms（Δ+17.38ms；模型 18.48ms ✓）
- comp-ns=500 → exec 266.39ms（Δ+2.02ms；模型 R×500ns = 1.02ms，观察 1-2ms 带 ✓）
- 组合加性成立：(50,500)→270.32ms，(200,500)→282.15ms（与基线+两项之和差 ≤1.7ms）
计费模型 n_dist×adc-ns + R×comp-ns 得到引擎级验证；ADC 项按实际 ADC 距离次数计费，
comp 项按 rerank 次数计费。QPS 随旋钮同步下移（3.8→3.5）。

## A2 vs A1 布局代价（G3 遥测，2026-09-13）

G3 = A2 自洽门 PASS：A2 dump md5 == A1 == ref（7a50ae34），同码同图同游走。
- A1（码区 pin）：code-pages 0.0/q，exec 264.4ms/q，QPS 3.8
- A2（码内联图页）：code-pages 26,061.8/q，exec 1865.8ms/q（7.1×），QPS 0.5（7.6×）
- dist/hops/rerank 三者逐位同（92,393.2 / 2065.2 / 2048.0）——walk 不变，代价全在
  码页缺失。AiSAQ"码驻留"叙事的引擎级证据：布局选择决定 rerank 数据局部性。
A2 推送实测 8.2min（71MB/s，flush 后 DER 状态干净，比 A1 首推 4×）。
