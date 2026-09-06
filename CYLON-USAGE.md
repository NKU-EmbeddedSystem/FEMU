# Cylon CXL-SSD 模拟环境使用说明

> 本机部署与联调实测文档（2026-09-02 验证通过）。原理与代码分析见 `CYLON.md`，
> 本文只讲"怎么用"。环境：Dell 裸金属（128G/2 NUMA）→ FEMU 9.0.1-Cylon → Ubuntu 22.04 guest。

---

## 1. 环境总览

| 层 | 内容 |
|---|---|
| L0 宿主机 | 内核 `6.4.6-cylon`（deb 修订 -5，defconfig+9 项定制，见 §2）；memmap=2G!128G 预留 pmem（FEMU 缓存片） |
| FEMU | `/home/liz/FEMU/build/qemu-system-x86_64`，femu_mode=6（CXLSSD），48G CXL 2.0 Type-3 设备 + 512M DER 缓存（FIFO） |
| L2 guest | `~/images/ubuntu22.qcow2`，Ubuntu 22.04 jammy + 同一个 6.4.6-cylon deb，8 vCPU / 16G，账号 liz（SSH 公钥登录） |
| CXL 设备内部 | 48G "NAND"（宿主内存模拟，模拟闪存延迟 40µs读/200µs写）+ 512M DRAM 缓存（DER 页级 EPT 重映射，命中 ~1µs） |

关键目录：
- FEMU 脚本与日志：`/home/liz/FEMU/femu-scripts/`（运行日志 `log`）
- 内核源码/构建/产物：`/var/tmp/cylon/`（源码 `CylonLinux/`，O= 构建 `build-cylon/`，deb、.config 都在这）
- guest 内工具：`/usr/local/bin/setup_cxl.sh`、`/usr/local/bin/cxl_warmup_new`

## 2. 自定义内核 6.4.6-cylon（编译、安装、切换）

为什么要自编内核：**DER 依赖宿主机 KVM 补丁**（`KVM_GET_LINEAR_EPT` ioctl、`KVM_MEMSLOT_DUAL_MODE`），
FEMU 的 `der_kvm.c` 直接调用这些接口；上游内核没有，必须用 Cylon artifact 提供的内核源码（6.4.6 vanilla
+ KVM DER 补丁）编译。L0 和 L2 装的是同一个 deb。

源码：`/var/tmp/cylon/CylonLinux`（GitHub MoatLab/Cylon → CylonLinux），O= 构建目录 `/var/tmp/cylon/build-cylon`。

### 2.1 配置 = defconfig + 9 项

| 选项 | 用在哪 | 缺了会怎样 |
|---|---|---|
| `MEGARAID_SAS` | L0 | 本机 LSI MegaRAID SAS-3 3108 阵列卡无驱动 → 所有盘不可见，卡 initramfs |
| `BNXT` | L0 | BCM57412 10G 网卡无驱动 → 10G 口无网（注意：6.4 里符号名是 `BNXT`，`BNXT_EN` 是 6.5+ 的名字） |
| `X86_X2APIC` | L0 | 固件开了 x2APIC，缺它只有 BSP 1 个核上线（**nproc=1**、KVM 报 "recommended cpus (1)"），vCPU 全挤 1 核 |
| `IRQ_REMAP` + `INTEL_IOMMU` | L0 | x2APIC 需要中断重映射，配套开 |
| `CXL_REGION_INVALIDATION_TEST` + `CXL_MEM_RAW_COMMANDS` | L2 | `cxl create-region` 报 `failed to commit decode: No such device or address`（ENXIO） |
| `FS_DAX` + `FS_DAX_PMD` | L2 | devdax 一切 mmap 报 EINVAL（dmesg: "vma is not DAX capable"） |

另外 `.config` 里 `CONFIG_LOCALVERSION="-cylon"`（内核版本名）。

⚠️ **defconfig 派生配置的通病**：x86_64 defconfig 是最小配置，不含这台机器的阵列卡/网卡/固件特性。
每次换内核基线（比如升级到 6.12）都要**重新全面核对**这九项 + 本机其它硬件驱动，缺一个就启动失败。

### 2.2 编译出 deb

```bash
cd /var/tmp/cylon/CylonLinux
make O=/var/tmp/cylon/build-cylon defconfig
./scripts/config --file /var/tmp/cylon/build-cylon/.config \
    -e MEGARAID_SAS -e BNXT \
    -e X86_X2APIC -e IRQ_REMAP -e INTEL_IOMMU \
    -e CXL_REGION_INVALIDATION_TEST -e CXL_MEM_RAW_COMMANDS \
    -e FS_DAX -e FS_DAX_PMD \
    --set-str LOCALVERSION "-cylon"
make O=/var/tmp/cylon/build-cylon olddefconfig
make O=/var/tmp/cylon/build-cylon -j$(nproc) bindeb-pkg
# 产出 /var/tmp/cylon/linux-{image,headers,libc-dev}-6.4.6-cylon_*_amd64.deb
# deb 修订号（-2/-3/-4/-5）由 build-cylon/.version 计数器控制，每跑一次 bindeb-pkg +1
```

（构建依赖：`apt install build-essential libncurses-dev bison flex libssl-dev libelf-dev bc dwarves`）

### 2.3 安装与切换（L0）

```bash
sudo dpkg -i /var/tmp/cylon/linux-{image,headers}-6.4.6-cylon_*_amd64.deb
# dpkg 会自动 update-grub 生成默认菜单项，但默认项没有 memmap 参数 → 用 40_custom 加了独立菜单项
```

### 2.4 GRUB：memmap 菜单项（L0 一次性配置）

`/etc/grub.d/40_custom` 追加（实际内容见 `cat /etc/grub.d/40_custom`）：

```bash
menuentry 'Ubuntu, with Linux 6.4.6-cylon (CXL-SSD memmap)' --class ubuntu --class gnu-linux --class gnu --class os {
      insmod gzio
      insmod part_gpt
      insmod ext2
      search --no-floppy --fs-uuid --set=root fd4b98c2-142e-48f1-a31d-30156b8c5fbd
      linux   /vmlinuz-6.4.6-cylon root=/dev/mapper/ubuntu--vg-ubuntu--lv ro memmap=2G!128G
      initrd  /initrd.img-6.4.6-cylon
}
```

`memmap=2G!128G`：把物理内存 [128G,130G) 预留成 pmem（region0），即 FEMU 缓存片的物理后备；
不加则 FEMU 缓存片落在可回收内存上，会崩。

⚠️ **缓存后端必须是 devdax，不能是 `/dev/pmem0` 块设备**（2026-09-03 实测死锁）：块设备/文件的
mmap 一律走**页缓存**（DRAM）——cache 写脏后内核回写会写保护 PTE，FTL fill 的 memcpy 陷入
1.7M/s 写缺页循环，持锁连锁冻结整个 QEMU（mlock 防不了回写）。一次性转换（宿主机）：

```bash
sudo ndctl destroy-namespace namespace0.0 --force
sudo ndctl create-namespace -r region0 --mode=devdax   # → /dev/dax0.0 (2014M, resource 0x2002200000)
```

run-cxlssd.sh 已固定 `cache_backend_dev=/dev/dax0.0`；FEMU 启动时自动读 sysfs `resource`
作为权威 hpa_base（cache.c 解析顺序：sysfs resource > pagemap > 参数，resource 文件内容带
`0x` 前缀故用 strtoull 解析）。devdax mmap = 一次性 remap_pfn_range，无页缓存、无 per-page
fault，HPA 永久稳定——这是 DER guest 直达 EPTE 的前提。

⚠️ **感叹号陷阱**：在交互 shell 里用 heredoc/交互输入写含 `memmap=2G!128G` 的内容时，bash 会把
`!128G` 当历史扩展吃掉（写进去只剩裸 `memmap`）。修复用单引号 sed：
`sudo sed -i 's/memmap$/memmap=2G!128G/' /etc/grub.d/40_custom`，改完 `sudo update-grub`。

⚠️ `GRUB_DEFAULT=0`（默认进原 7.0.0-30 内核，安全）。进实验内核是**一次性引导**：

```bash
sudo grub-reboot 'Ubuntu, with Linux 6.4.6-cylon (CXL-SSD memmap)' && sudo reboot
```

### 2.5 装进 guest（L2）

同一个 deb。宿主机起 HTTP 服务派发（L0 端口 8000 被占，用 **8093**）：

```bash
cd /var/tmp/cylon && python3 -m http.server 8093 &
# guest 里：
curl -O http://<L0-ip>:8093/linux-image-6.4.6-cylon_*.deb && sudo dpkg -i *.deb
```

guest 默认启动项已是 6.4.6-cylon。装完先 `nproc`（应 40），再启动 FEMU。

## 3. 启动流程（每次冷启动）

```bash
# 0) 宿主机应在 6.4.6-cylon 内核上（uname -r 确认；不在则一次性引导，见 §2.4）：
sudo grub-reboot 'Ubuntu, with Linux 6.4.6-cylon (CXL-SSD memmap)' && sudo reboot
# 验证：nproc 应为 40（若是 1 说明进错内核或缺 X86_X2APIC，vCPU 会饿死）

# 1) 启动 FEMU（前台，48G 设备）：
cd /home/liz/FEMU/femu-scripts && sudo ./run-cxlssd.sh 49152

#    免值守重启（/etc/sudoers.d/99-liz-femu NOPASSWD 已装时可用；脚本以 liz 跑，
#    内部各 sudo 命令免密）：在专用 tmux 会话 femu 里杀旧 QEMU + 重启，随时
#    `tmux attach -t femu` 看 QEMU 控制台（QEMU 退出后会话留 shell 可查现场）：
/home/liz/FEMU/femu-scripts/femu-restart.sh 49152

# 2) guest 自动 boot（~40s）。宿主机另开终端 SSH 进入：
ssh -p 8080 liz@localhost

# 3) guest 里完成 CXL 设备上线（每次 FEMU 重启都要跑一次）：
sudo env LD_LIBRARY_PATH=/usr/lib /usr/local/bin/setup_cxl.sh devdax
#    （ndctl 需要 /usr/lib 里的旧版 libdaxctl/libndctl；不带 env 会报库缺失）
```

⚠️ **FEMU 终端（-nographic 控制台）的按键**：`Ctrl-C` 会直接杀掉 QEMU
（"terminating on signal 2"），`Ctrl-A X` 退出 QEMU，`Ctrl-A C` 切 QEMU monitor。
**要给 guest shell 发 Ctrl-C 请用 SSH**；控制台只拿来看启动日志/登录救急。

## 4. Guest 访问

```bash
ssh -p 8080 liz@localhost    # 公钥免密（L0 的 id_ed25519 已入 guest authorized_keys）
```

- 每次重启 FEMU 后首次连接会提示 host key 变化，属正常（`-o UserKnownHostsFile=/dev/null` 可绕过）
- guest 出网走 SLIRP（NAT 经宿主机），apt 源已配阿里云（`mirrors.aliyun.com`）
- 宿主机校园网偶发镜像抽风不影响 guest（guest 走公网 CDN）

## 5. CXL 设备上线与三种模式（setup_cxl.sh）

```bash
sudo setup_cxl.sh devdax       # 应用直接 mmap("/dev/dax0.0")，load/store 语义（chmod 666 自动放开）
sudo setup_cxl.sh system_ram   # 变成 CPU-less NUMA 节点 1（48G），配合 numactl 使用
sudo setup_cxl.sh fsdax        # 块设备模式，可 mkfs.ext4 + mount -o dax
```

脚本幂等（region 已存在会跳过），每次 FEMU 重启后跑一次即可。
注意：**切换模式会清掉设备上已有的数据语义**（devdax↔system-ram 切换数据仍在，但建议实验前重新 warmup）。

### 5.1 查看设备

```bash
cxl list -r 0           # region0: decode_state 应为 commit，resource=0x2290000000
daxctl list             # dax0.0 当前模式
numactl -H              # system-ram 模式下应见 node1: 48G, 0 CPUs, distance 20
lspci | grep -i accel   # CXL Type-3 (0000:0d:00.0)
```

### 5.2 各模式典型用法

```bash
# system-ram：把 CXL 当扩容内存（最常用）
numactl -m 1 redis-server --port 6379        # 内存全落 CXL 节点
numactl -i 0,1 ./app                          # 本地+CXL 交织分配
numastat                                      # 查看各节点实际分配量

# devdax：内存语义编程（PMDK / 自研工具）
fio --ioengine=mmap --filename=/dev/dax0.0 --size=48G \
    --rw=randread --bs=4k --iodepth=1 --runtime=10    # fio 的 devdax engine 不可用，用 mmap + 显式 size

# fsdax：文件系统
sudo setup_cxl.sh fsdax && sudo mkfs.ext4 /dev/pmem0.0 && sudo mount -o dax /dev/pmem0.0 /mnt
```

⚠️ node1 无 CPU（`numactl -N 1` 绑 CPU 会失败）——计算永远跑在 node0 的 8 核上，
CXL 只提供内存容量，这是标准 CXL 内存扩展形态。

## 6. 性能特性（实验设计前必读）

实测（2026-09-02）：

| 路径 | 延迟 | 说明 |
|---|---|---|
| DER 缓存命中（direct EPTE） | **~1µs**，1.14M IOPS | 512M 缓存内驻留页，硬件直映 |
| 缓存缺失（trap→FTL→NAND） | **~50-250µs** | 模拟闪存延迟，~10K IOPS |
| 48G 全量写 warmup | 22.6s（`cxl_warmup_new 48 /dev/dax0.0`） | |

要点：
1. **512M 缓存对 guest 透明**：设备内部自动管理（FIFO），guest 无法控制哪些页享受快通道
2. **设备写满 48G 后，新写入触发 GC 爬行**（读不受影响）——基准测试优先用读负载；
   若要重测写，重启 FEMU 复位 FTL
3. **数据不跨 FEMU 重启**：48G 内容在 FEMU 进程内存里，QEMU 一退全丢；
   guest 热重启（sudo reboot，FEMU 不死）则数据保留
4. 空闲时几个 vCPU 100% 是 `--overcommit cpu-pm=on` 的 mwait 自旋，正常现象，非泄漏
5. 实验流程参考 artifact：warmup（触发冷 EPT fault、填 EPT 表）→ 负载。
   devdax 下：`sudo /usr/local/bin/cxl_warmup_new 48 /dev/dax0.0`

## 7. 验证 DER 是否工作

DER（Dynamic EPT Remapping）= FEMU 按页改写 KVM EPT：缓存内页直映宿主物理地址，
换出页回到 MMIO trap。行为指纹：**随机读延迟双峰**（~1µs 命中峰 + ~200µs 缺失峰）。

```bash
# devdax 模式下，256M 窗口（缓存驻留）vs 48G 全域（必然 miss）对比：
fio --ioengine=mmap --filename=/dev/dax0.0 --size=256M --rw=randread --bs=4k \
    --time_based --runtime=10 --iodepth=1     # 稳态 ~1.14M IOPS = DER 直映生效
```

宿主机侧证据（`femu-scripts/log`）：启动时 `Cylon DER-KVM: KVM_SET_USER_MEMORY_REGION:
slot 42` + `init_leaf_ept done`。逐页 EPTE 日志需 `FEMU_DER_VERBOSE=1` 重启 FEMU（默认静默，
否则 48G warmup 会刷 600 万行日志）。

## 8. PNM ANNS 工作流（Phase A，2026-09-03 验收通过）

设备内 ANNS 引擎（`hw/femu/cylon/pnm.c`）：HNSW 遍历在 FEMU 侧线程跑，guest 只管写数据+收发信箱。
信箱在窗口末尾（end-4096），结果区 end-20480；协议 = job+PENDING → 引擎跑 → resp+DONE → IDLE。

### 8.1 两种运行模式

| 模式 | 条件 | guest 访问路径 | 用途 |
|---|---|---|---|
| **完整 DER** | 无 `/tmp/femu-der-disable` | 未缓存页 MMIO trap → FTL（首触 NAND 延迟+缓存插入+EPTE 直映）；命中页 EPT 直映 devdax pmem | 时序实验（**已验证等价**，见 §8.4） |
| **验收模式** | `touch /tmp/femu-der-disable` + `run-cxlssd.sh` 里 `skip_ftl=1` | 整窗普通 RAM memslot（slot 42，flags=0）EPT 直映 logical_space，零 exit | 正确性对拍（快且稳） |

⚠️ 完整 DER 的 bulk staging 约束（均已解决，2026-09-03 验证）：① 每页必须**标量（≤8B volatile）
store 首触**——KVM 仿真器不解 VEX/AVX，glibc ifunc memcpy 首个向量 store 首触 trap 页会 SIGILL
（pnm_client stage_file 已内置逐页标量首触，之后 memcpy 走直映 EPTE 畅通）；② cache 后端必须
devdax（§2.4，否则页缓存回写死锁）；③ **灌进 guest 的所有输入文件先 md5 对账**——零填充的
queries 会得到 recall=0 而 QPS/延迟一切正常的假结果（判别签名：引擎报的距离==向量范数 |v|²）。
满足后 staging 495MB 冷 26.5s（127K 页全走 trap→FTL→flip）、暖 0.3s，无任何挂死。

### 8.2 跑一次查询对拍

guest 工件（qcow2 持久，随镜像保留）：`/var/tmp/cylon/anns/`：
`pnm_client.static`（md5 f55d5ad…，带 `-o` dump 选项）、`cyh1_sift1m.blob`（495MB, CYH1, fp16）、
`queries_fp16.bin`（10000×128）、`gt_u32.bin`（10000×10）。

```bash
# 验收模式全流程（FEMU 起好 + devdax 上线后）：
sudo /var/tmp/cylon/anns/pnm_client.static \
    -f /var/tmp/cylon/anns/cyh1_sift1m.blob \
    -q /var/tmp/cylon/anns/queries_fp16.bin \
    -g /var/tmp/cylon/anns/gt_u32.bin \
    -n 1000 -k 10 -e 100
# 预期：recall@10 = 0.9896，QPS ≈ 380，p50 ≈ 2.7ms（无时序注入裸跑基线）
```

阶段输出：ping → staged 495MB（~1s）→ BIND（引擎快照整份索引进本地内存，~1s）→
staged queries → 逐条 SEARCH。FEMU 日志能看到 `index bound` / `search job … found`。

### 8.3 host 侧对拍工具（/var/tmp/cylon/anns/tools/，不在仓库）

| 工具 | 用途 |
|---|---|
| `engref.c` → engref | pnm.c 遍历**逐字复刻**直跑 blob 文件，产 recall 基线 + 每 query 结果 dump；引擎任何改动先在这里对拍 |
| `revq.c` → revq | 反推"guest 实际当成了哪条查询"——遍历全部 10000 向量找精确重现目标 top-10 的那条（信箱竞态就是它定案的） |
| `export_cyh1.py` | hnswlib .idx → CYH1 blob（label 空间重映射，见 memory：hnswlib label 错位） |

dump 格式（client `-o` 与 engref 第 7 参一致）：每 query `u32 n_found + k×u32 id`，`cmp` 直接比。

### 8.4 验收数字（2026-09-03）

| 指标 | 验收模式（plain memslot） | **完整 DER**（trap/flip 全开） |
|---|---|---|
| recall@10（n=1000, ef=100） | **0.9896**，与 engref 逐字节一致 ×2 轮 | **0.9896**，dump 与 engref **md5 相同**（c09710e0…） |
| staging 495MB | ~1s | 冷 26.5s / 暖 0.3s（flip 常驻） |
| QPS / p50 / p99 | 380–392 / 2.6–2.7ms / 3.2–3.3ms | 399–478 / 2.0–2.6ms / 3.0–3.1ms |
| 引擎工作量 | dist 3516.7/query，hops 109.1/query（与 host 完全一致） | dist 3516.7，hops 109.1（同——引擎看到的索引数据逐位相同） |

完整 DER 跑法（guest，`/tmp/pnm_client` 由 host `/var/tmp/cylon/anns/tools/pnm_client.c` 重推重编，
数据在 `/var/tmp/anns/`，**先 md5 对账**）：

```bash
cd /var/tmp/anns && sudo /tmp/pnm_client -f cyh1_sift1m.blob -q queries_fp16.bin \
    -g gt_u32.bin -n 1000 -k 10 -e 100 -o guest_full_der.dump
# host: scp 回来后 cmp engref_ef100.dump guest_full_der.dump → 应逐字节相同
```

## 8.5 cache-aware 搜索模式（Phase B，2026-09-04 验证通过）

Phase A 引擎 BIND 时把整份索引快照进本地内存（local copy）；Phase B 起默认 **cache-aware**：
每次图读（向量/邻接表）都走缓存层级，`bufsz/policy/prefetch` 真正塑造搜索延迟。

- **模式开关**（`pnm.c pnm_use_local_copy`）：默认 cache-aware；`PNM_LOCAL_COPY=1`（FEMU env）
  强制 Phase A local copy；验收模式（`cxl_skip_ftl=1`）自动强制 local copy（无 FTL 无缓存可走）。
- **`pnm_graph_read` 语义**：slot 命中零开销；缺失 = 完整插入路径（策略 insert + 驱逐）+ 计一次
  `pg_rd_lat`（40µs），resp 的 `n_pages` = 每 query miss 数（SIFT1M/ef=100 冷缓存平均 251.9，
  跨 query 变暖：首 query ~2700 → 尾部 ~180）。窗口越界读 = 零填充 + `!! graph read past window end`
  告警（不 crash——脏数据由 dump 门禁兜底）。
- **尾页自钉**（`DER_TAIL_PIN_PAGES=32`，der_kvm.c）：mailbox/results/query 尾页在**首次 trap 时**
  自钉 direct（dual-mode leaf 首 walk 才物化，init 预写无效），永不入缓存；cache insert/flush 双守卫
  拒绝（日志 `insert REFUSED tail` / `flush REFUSED`），der_kvm 侧 TAIL FLIP tripwire。
- **slot 所有权 = free-slot LIFO 栈**（cache.c）：slot 入服务仅经 pop（或驱逐直传），回收仅经
  `cylon_cache_reset`——所有权独占。**旧 sentinel-0 + `next_slot%nr_slots` 盲分配器会 alias**：
  驱逐继承 slot 0 时分配器二次触发，把在用 slot 发给第二个 entry，一页 fill 覆盖另一页 →
  引擎把向量 fp16 位型当邻接 id（如 0x51E04A00≈(47.0,8.0)）→ 出窗读 → `logical_space+off`
  兜底越界 → QEMU SIGSEGV；alias 随驱逐自愈，故表现为**随机中途崩溃**（ca256/ca256b 幸存、
  第三 run 在 search 911 爆）。教训：**跑完没炸 ≠ 正确，dump 门禁 + tripwire 缺一不可**。
- **`PNM_OP_CACHE_FLUSH`（op=3）**：同 FEMU 重跑前冷复位（pnm_client staging 前自动提交）：
  全驻留 slot 回写 NAND → 经各策略自身 evict 路径清树 → 全窗 EPTE 复位 trap → 重建 free 栈。
  不做则 staging 走残留 direct EPTE 绕过 FTL（~3.2s + BIND bad magic，见 §9）。**只复位 cache，
  不清 FTL map**——重跑 staging 走 read 计费（见下），数据一致性由 NAND 持久保证。
- **staging 时序三分法**（495MB = 126,720 页，判读重跑健康度）：
  **~26.5s** = 208µs/页 NAND program 计费（FTL map 空，冷启动首轮）｜**~6s** = 47µs/页 read 计费
  （map 已映射，FLUSH 后重跑的**正常值**）｜**~3.2s** = 零计费 bypass（**坏签名**）。
- **D2 设备发起 staging（`PNM_OP_STAGE=4`，2026-09-06 实现，见 CYLON-TYPE2.md §4）**：
  客户端 `--stage=dev`（cpu_search 与 pnm_client 同款；wiki_exp.sh 第 4 参 `ft|dev`）。引擎把
  mapped 页从介质批量重填 512MB 缓存（lpn 升序 = first-touch 同序 → 终态逐位同），按
  `/tmp/femu-stage-bps` 计费（**缺省 2GB/s 计费、文件存在且 0 = 不计费**——有意区别于其他
  /tmp 旋钮 absent=off）。wiki 36GB 时序四分法：**~60s** = 真实填充下限（knob=0）｜**~18.4s**
  = 2GB/s 计费（缺省操作点）｜**~2.6s** = 14GB/s｜**33min/542s** = first-touch（fresh boot
  回退路径覆盖 0 → 自动 FLUSH + stage_file，客户端日志 `falling back to first-touch`）。
  覆盖数 = resp.n_found；老引擎 ENOSYS → 回退；超时不回退（引擎 mid-job）。
- **使用配方（分支角色，2026-09-06 定）**：活跃实验线 = `cylon-v9.1-type2`
  （origin/dev-cxl-type2）；`cylon-v9.0.1`（origin/cxl-accelator）= Type-3 论文快照，
  冻结。**Type-3 实验直接在 type2 分支引擎上跑**——分支只是叙事容器，配置开关是
  旋钮：BI 旋钮不建（缺席=off）+ 不调 STAGE op 即 Type-3 配置（es T 系列 = 活证，
  dump 全逐字节 = engref）。每 boot 首跑必须 ft（建 maptbl，冷 33min 一次）；之后
  所有点用 `dev` 加速（终态已证逐位同；E-S 里 Type-3 的 staging 数字除外——那是
  被测对象，必须 ft 忠实跑）。
- **canonical tie-break**：~14% 查询 top-10 含相邻等距对，裸 qsort tie 序随堆内序漂移 →
  `pnm.c pnm_cmp_asc` 与 `engref.c pnm_cnd_asc` 都按 (d, id) 排序。**engref 参考已重生成**
  （2026-09-04，recall 0.9896→0.9898；旧 dump 系早期 engref 二进制产物，备份 .pre-tiebreak）。

### 8.6 三连验证（修复后，2026-09-04，bufsz=256M/FIFO/ef=100）

| | ca256 冷启动 | ca256b 同 FEMU 重跑 | ca256q2 换查询集重跑* |
|---|---|---|---|
| FLUSH job | ✓ →0 | ✓ →0 | ✓ →0 |
| staging | 26.4s（program 计费） | 6.0s（read 计费，正常） | 6.0s |
| recall@10 | **0.9898** | **0.9898** | **0.9882**（=engref_q2） |
| dump vs engref | **逐字节一致** | **逐字节一致** | **逐字节一致（engref_q2）** |
| engine/query | dist 3516.7 hops 109.1 | 同左（逐位） | dist 3539.3 hops 109.1（=engref_q2） |
| misses/query | 251.9（cache 冷） | 251.9（cache 冷） | 255.0 |

FEMU 日志：3×1000 search 全完成、3 FLUSH、**零 tripwire**。QPS 63-65（cache-aware 全层级计费）。
\* 换 test[1000:2000] 子集——**同数据重跑验证不了内容一致性**（bypass 时旧数据照样逐字节对），
换数据才能证明 FLUSH 后引擎读的是新灌数据（q0 top-10 与新参考一致即证）。

### 8.7 DSE：cache-aware QPS-缓存容量曲线（2026-09-04，FIFO/ef=100/n=1000）

免值守重启（`femu-scripts/femu-restart.sh` + sudoers NOPASSWD，见 §3）跑完，每点门禁全过
（recall 0.9898 / dump 逐字节 = engref / 1000 search job / **零 tripwire**）：

| bufsz | 索引驻留率 | misses/query | p50 | p99 | QPS | QPS/miss-model 自洽 |
|---|---|---|---|---|---|---|
| 512M | 100%（495MB 全驻留） | **0.0** | 4.1ms | 5.0ms | **251.7** | 纯 slot 命中开销 |
| 256M | ~52% | 251.9 | 13.8ms | 70.3ms | 64.8 | 252×40µs=10ms ✓ |
| 128M | ~26% | 1252.0 | 60.8ms | 92.0ms | 17.0 | 1252×40µs=50ms ✓ |
| 64M | ~13% | 1930.6 | 91.3ms | 122.9ms | 11.5 | 1931×40µs=77ms ✓ |

要点：
- **正确性与时序解耦坐实**：四个配置下 dist 3516.7/hops 109.1 逐位一致，dump 全部逐字节相同。
- **全驻留上限 251.7 QPS**（vs Phase A local-copy 380-392）：cache-aware 每次图读都过
  slot lookup 的架构税 ~35%，这是"诚实"的近存引擎数字，之后优化 prefetch/lookup 的基线。
- miss 计费主导段：QPS ≈ 1/(misses×40µs) 精确成立（每点 p50 与模型差 ≈ 常量开销），
  说明 miss 路径计费干净、无隐藏时延。
- 256→128 驻留率减半 miss 涨 5×（252→1252）：跨 query 图页复用率随容量坍塌（1000 条随机
  查询工作集 >> 缓存时 FIFO 无法保留热点页），S3FIFO/CLOCK 的提升空间正在这里（待跑）。
- staging 时长四点恒 26.4s（program 计费与容量无关；staging 期驱逐回写不加计费）。


## 8.8 CPU 侧 baseline 与 stale-EPT-TLB 修复（2026-09-04）

**CPU-baseline** = "CXL SSD 只暂存、CPU 全算"对照：`tools/cpu_search.c`（engref 遍历逐字复刻，
`st.graph = win`）在 guest vCPU 上跑，全部图读走 CXL 窗口（EPT-direct 命中 / trap→FTL miss），
staging/FLUSH 协议与 pnm_client 一致；`tools/cpu_exp.sh <tag> [n]` 一键跑。构建两档：
`-O2 -fno-tree-vectorize`（标量安全，VEX 不能首触 trap 页）/ `-O3 -mavx2`（**仅全驻留后安全**，
纯 direct 读；无 -ffast-math 故 FP 归约不向量化，结果仍与 engref 逐位一致）。

**heap_pop_max 转录 bug（512M 确定性错位真凶，2026-09-04 修复）**：cpu_search 首跑即确定性
错位（query0 第 3 名恒 695756、dist 680/hops 21 vs 正确 3516.7/109.1、recall 0.6540），曾被
误诊为 stale TLB。真凶：res（top_candidates）max-heap 的 siftdown 挑了较小孩子 → 堆顶假 worst
→ `c.d > worst` 提前 break → 早停。定位链：winverify 逐字节对账（窗口干净）→ `-R 2` 双 pass
恒同（非瞬时）→ 遍历核心函数级 diff engref vs cpu_search → 一字符之差。教训：**确定性错位+早停
先 diff 遍历代码，别急着怪环境**。cpu_search 现有诊断旗标：`-T` hop 级 trace、`-S` 跳过
FLUSH+staging（AVX2 档专用：AVX2 不能 staging，VEX 首触 trap 页会挂；先标量跑负责 FLUSH+stage，
再 `cpu_search_vec -S` 纯 direct 读）、`-R` 多 pass。

**stale EPT TLB（修复保留，适用于 <全驻留）**：DER leaf 改写走用户态共享 alias，KVM 不感知、
无 INVEPT → 驱逐后 vCPU TLB 里陈旧 direct 翻译把 victim 页读到旧 slot 现任数据。引擎侧读不经
EPT 从未中招（DSE 全对）；guest 直读数据页（cpu_search）在**有真驱逐的容量点**（256M 及以下）
踩雷。~~512M 全驻留也中~~——**此说已撤销（2026-09-04 定案）**：512M 无 flip 无翻案场景，当时的
确定性错位实为 cpu_search 自身 `heap_pop_max` 转录 bug（见下）。签名（真 stale TLB）：驱逐场景
下确定性错位、dist/hops 暴跌、垃圾 id 出窗 SEGV；winverify 事后全对（TLB 早被冲掉，权威 leaf
没错、锅在 TLB）。

修复 = 新 ioctl + 调用点：

| 层 | 内容 |
|---|---|
| CylonLinux（deb **6.4.6-9**） | `KVM_DER_FLUSH_TLB _IO(KVMIO,0xdf)`（kvm_ext.h）→ kvm_main.c case → `kvm_flush_remote_tlbs(kvm)` |
| FEMU der_kvm.c | `der_kvm_flush_tlbs(ctx, guest)`：ENOTTY 时警告一次并按旧行为降级 |
| cache.c insert | 驱逐 flip-trap 之后、slot 复用 fill **之前** flush（stale 读会落入正确 FTL 路径） |
| cache.c reset | FLUSH op 尾部 flush（防 reset 后 stale 项让 staging bypass） |

**FEMU_DER_FLUSH 模式**：`0` 关 / `1` 默认=仅 guest 发起的 miss flush（staging + cpu_search；
引擎 miss 不 flush——host 读不经 EPT，DSE 引擎模型保持零 IPI 污染）/ `2` 全开（CPU+引擎
collab 模式必用：引擎驱逐同样会毒化并发 guest 读者）。
L0 装内核（deb 版本号同名，40_custom 菜单项不变）：
`sudo dpkg -i /var/tmp/cylon/linux-image-6.4.6-cylon_6.4.6-9_amd64.deb && sudo grub-reboot 'Ubuntu, with Linux 6.4.6-cylon (CXL-SSD memmap)' && sudo reboot`。

### 8.8.1 CPU-baseline 容量扫描（2026-09-04，FIFO/ef=100/n=1000，门禁全过）

四个容量点 dump 全部**逐字节=engref**，recall 0.9898，dist 3516.7/hops 109.1 逐位一致
（DER 窗口读路径 guest 直读版端到端定案）：

| bufsz | recall | dump | QPS | p50 | p99 | exec/query | ~misses | 引擎 QPS（同点） | 引擎 misses |
|---|---|---|---|---|---|---|---|---|---|
| 512M | 0.9898 | 逐字节同 | **409.3**（AVX2 **472.3**） | 2.51ms | 3.0ms | 2.44ms | 0 | 251.7 | 0 |
| 256M | 0.9898 | 逐字节同 | **70.5** | 12.2ms | 73.9ms | 14.2ms | ~302 | 64.8 | 251.9 |
| 128M | 0.9898 | 逐字节同 | 16.0 | 64.4ms | 98.3ms | 62.4ms | ~1560 | 17.0 | 1252 |
| 64M | 0.9898 | 逐字节同 | 10.6 | 99.3ms | 135.0ms | 94.7ms | ~2369 | 11.5 | 1930.6 |

要点：
- **交叉点在 256M 附近**：全驻留/浅 miss 区 CPU-baseline 快于引擎（引擎每次图读付 slot-lookup
  架构税），深 miss 区两者收敛到 1/(misses×40µs)，引擎微胜（FIFO 动态下 CPU 路径 miss 略多）。
- CPU 侧 miss 数为 exec/40µs 推算（client 不可见）；比引擎同点略高（同遍历路径、同策略但
  FIFO 时序差），量级一致。
- **stale-EPT-TLB flush 首个真实驱逐验证通过**：256M 及以下有真驱逐，dump 全对 = 无毒化。
- 踩坑新增：**verify_window 的 libc memcmp ifunc→AVX2 首触 trap 页 = SIGILL**（VEX 约束的
  libc 变体，首轮 256M 因它把进程杀在 fclose 前、dump 截成 40,960B=10×stdio 缓冲）；已改
  volatile 标量字节循环。AVX2 档只在全驻留后安全（staging 期 trap 页会挂），配合 `-S` 使用。

## 8.9 协同模式（CPU+引擎共算，v1 设计定稿 2026-09-04）

**目标**：查询流拆分 f 给 guest CPU worker（cpu_search 本地遍历），1-f 给 FEMU 引擎（信箱
SEARCH 任务），两端点 = CPU-baseline（f=1）/ 完整卸载（f=0），内点 = 协同。这是"CPU/设备
算力、频差、SSD 带宽可调"谱系的横轴；容量（bufsz）是纵轴。

### 8.9.1 v1 架构（静态块拆分）

| 件 | 设计 |
|---|---|
| 载体 | `cpu_search -F <frac>`（单进程双线程）。f=0/1 退化为纯引擎/纯 CPU，端点即内点同代码 |
| CPU worker | pthread，跑 `[0, n_cpu)` 的本地遍历（现有 pnm_search，独占 st 全局态） |
| 引擎 feeder | 主线程，信箱逐条提交 `PNM_OP_ANNS_SEARCH`（a0=qoff+qi·dim·2），单 outstanding，读结果区 k×(id,dist)+n_found 填合流数组；f<1 时先 BIND |
| 合流 | `all_ids[n][64]`/`all_d[n][64]` 每 query 单写者 → pthread_join 后按序写 dump、算 recall |
| 拆分 | 静态块：`qi < n_cpu = round(f·n)` → CPU，其余引擎。确定性、可归因，f 模型 T(f)=max(f·n/Q_cpu, (1-f)·n/Q_eng) |
| 正确性门禁 | **任意 f 下合流 dump 逐字节=engref**（两端各自已证逐字节=engref → 合流天然成立，除非 DER 协同路径引入毒化） |

### 8.9.2 DER 特有设计点

- **FEMU_DER_FLUSH=2 必开**（collab 专属）：引擎驱逐同样翻 leaf direct→trap，会毒化并发
  CPU reader 的陈旧 vCPU TLB——mode 1 只 flush guest-origin，漏引擎侧。run-cxlssd.sh 加
  `der_flush=` 配置行（sudo 会剥环境变量，配置进脚本内部 export）。
- **IPI 税**：每次驱逐一次 ioctl→全 vCPU INVEPT。512M 全驻留零驱逐零税（E1 干净谱系）；
  256M 起 CPU worker 的 p99 抖动是测量对象。
- **锁竞争**：CPU-origin miss 的 FTL fill 与引擎每次图读共用 cache lock——引擎 collab QPS
  vs standalone 的差值即竞争代价，测量项。
- **VEX**：<全驻留必须标量档（驱逐把页打回 trap）；512M 可 AVX2（零驱逐，`-S` 复用驻留窗）。
- **已知窄竞态**（standalone 256M 未观测到，collab 放大窗口）：guest 恰在驱逐翻转~flush 抵达
  之间的在飞读 = 撕裂读 → dump 门禁兜底；若复现，回炉修（victim 引用计数/延迟复用）。

### 8.9.3 实验计划与预测

- **E1（512M 全驻留，AVX2，零干扰谱系）**：f ∈ {0, 0.5, 0.65, 0.75}，预测合流
  T(f)=max 模型；f*=Q_cpu/(Q_cpu+Q_eng)=472/(472+252)≈0.65 → 理论合流 ~724 QPS
  （=两端点之和，资源独立时上限）。f=0 复现引擎 standalone 251.7（feeder 零开销校验）。
- **E2（256M，DER_FLUSH=2，标量）**：f ∈ {0, 0.25, 0.5, 0.75}，量化共享 FIFO 干扰
  （miss 膨胀）+ IPI 税 + 锁竞争；预测合流 QPS > max(端点) 但 < 端点之和。
- **v2 backlog**：work-stealing 动态均衡；CPU 侧 `-C` 每 dist 自旋（频率旋钮）+
  引擎侧每 dist 延迟注入（设备算力旋钮）；CCA ivshmem 控制面做任务环（CYLON.md §3.6）。

### 8.9.4 E1 实测：512M 全驻留 f 扫描（2026-09-04，FIFO/ef=100/n=1000/AVX2/-S）

门禁：**全部 6 点 dump 逐字节=engref，recall 全部 0.9898**——合流正确性在任何拆分比下成立。

| f | 合流 QPS | T_cpu (s) | T_eng (s) | dump 闸门 |
|---|---|---|---|---|
| 0（纯引擎） | 245.2 | — | — | ✅ |
| 0.25 | 327.0 | 0.537 | 3.057 | ✅ |
| 0.5 | 512.1 | 1.053 | 1.952 | ✅ |
| 0.65 | **665.4** | 1.420 | 1.502 | ✅ |
| 0.75 | 659.2 | 1.515 | 1.025 | ✅ |
| 1（纯 CPU AVX2） | 461.2 | — | — | ✅ |

- **max 模型成立**：端点速率预测 wall(f)=max(f·2.168, (1-f)·4.078)s/1000q。f=0.25 内点
  T_eng 3.057 vs 预测 3.059（差 0.1%）——引擎在内点跑出**与 standalone 完全相同的速率**，
  零干扰谱系定量坐实；各内点两侧速率偏差 ≤5%（run-to-run 方差量级）。
- **f\* = 0.653**（=461.2/(461.2+245.2)）→ 实测峰值恰在 f=0.65（665.4），f=0.75 贴随（659.2）
  ——max 模型的平台期如预测出现。
- **峰值 665.4 QPS = 1.44× 最快端点**（461.2），达"端点之和"完美聚合上限（706.4）的 94%。
  协同比纯 CPU、纯引擎都快的命题在零干扰谱系下**实测成立**。
- 标量 staging 点 336.6 QPS 低于上周期 409.3（-18%），但同 sweep 的 vec f=1（461.2 vs
  472.3）与 f=0（245.2 vs 251.7）均贴历史值（-2~3%）——偏差为标量档特有，E2 标量端点会
  再交叉核验。

### 8.9.5 E2 实测：256M 真驱逐协同（2026-09-04，FIFO/ef=100/n=1000/标量/DER_FLUSH=2）

门禁：**全部 5 点 dump 逐字节=engref，recall 全部 0.9898**——mode 2 下并发读无毒化。

| f | 合流 QPS | T_cpu (s) | T_eng (s) | dump 闸门 |
|---|---|---|---|---|
| 0（纯引擎） | 57.5 | — | — | ✅ |
| 0.25 | 75.8 | 5.005 | 13.198 | ✅ |
| 0.5 | **103.0** | 7.945 | 9.708 | ✅ |
| 0.75 | 94.1 | 10.625 | 6.036 | ✅ |
| 1（纯 CPU 标量） | 68.3 | — | — | ✅ |

- **端点交叉核验过**：f=1 = 68.3 vs 上周期 70.5（-3%，f=1 时 mode2≡mode1，E1 的标量异常
  未复现）；f=0 = 57.5 vs 上周期 64.8（**-11.3% = mode 2 纯 flush 税**：f=0 无 guest-origin
  miss，mode1 下零 flush，mode2 下 ~16.3K 次/秒 ioctl+INVEPT 广播全为纯开销）。
- **峰值 103.0 QPS（f=0.5）= 1.46× 最快端点**，达端点之和完美聚合上限（125.8）的 82%
  （E1 零干扰谱系是 94%——**干扰的代价 = 12 个聚合点**）。
- **干扰不对称**：f=0.25 时 CPU worker 整窗与引擎重叠，T_cpu +37%（IPI 税打在 vCPU 上）但
  被瓶颈侧吸收，组合无损（75.8 = 零干扰理想 98.8%）；f=0.75 时引擎吃满重叠窗，T_eng +39%。
  f=0.5 双侧各 +9~12%（对称退化），组合效率 89.6%。max 模型在 f=0.25/0.75 完全成立
  （100%/99%），只在双活跃窗最长的 f=0.5 出现真实干扰损耗。
- **协同的增益在真驱逐场景依然压倒性**：103.0 超过两个端点（57.5/68.3），CPU+设备共算
  命题在 256M 复现。
### 8.9.6 E2b 负对照：mode 1 下协同 = SIGSEGV（2026-09-04，"mode 2 必开"实测坐实）

配置：256M/FIFO/标量/collab f=0.5，唯一变量 der_flush=1。结果：**进程在 BIND 完成后
pass 起步即 SIGSEGV**（dmesg：`cpu_search segfault ... error 4`（not-present/user/read），
读 query fp16 向量元素 32 时炸，dump 0 字节；1/1 复现于首 run）。机制：引擎侧驱逐翻转
direct→trap 无 flush（mode 1 只 flush guest-origin），并发 vCPU 的陈旧 direct 翻译/被清
leaf 碰撞升级为 guest 无法 resolve 的 not-present 缺页——毒化不止是错字节，可以是硬崩。
**结论：collab 必须 der_flush=2**（E2 5/5 字节全对 vs mode1 1/1 段错误）。标量档 +
窗口内边界检查保证负对照无更长尾损伤（没伤到 NAND/FTL 状态）。n=1；即便偶发，也不
改变 mode 2 作为 collab 前提的结论。


### 8.9.7 设备算力阉割旋钮（compute_ns_per_dist，2026-09-04）

**动机**：FEMU 引擎是宿主 C 代码，实测算力 ≈ 0.53× 客户机 vCPU（512M 全驻留 per-dist：
引擎 ~1130ns vs CPU AVX2 ~602ns）——远强于真实 CXL SSD 控制器（1-2 个 ARM 小核，~1-10µs/
dist = 服务器核的 1/4-1/10）。设备的真实优势应当是**存储容量 + 媒体近端性**，不是算力。
论文诚实性要求阉割。

**语义**：PNM dist 阵列每距离计费 N ns，SEARCH job 收尾一次性 busy-wait 结算（"计算阵列
在 cache 旁、控制器核空闲"模型；与 miss burn 串行相加 = 保守不重叠模型）。total_ns 含该
延迟，客户端 QPS 直接反映。`0 = 未阉割（E1/E2 引擎原样）`。

**用法**：宿主 sentinel `/tmp/femu-compute-ns`（**每 job 读取，扫描免重启**；run-cxlssd.sh
55 行 `comp_dly=` 配置行开机写入）。pnm.c 在 pnm_handle_search 遍历后结算。

**校准（E0）**：512M 全驻留 / f=0 纯引擎 / per_dist ∈ {0,250,500,1000,2000}，预测
QPS ≈ 1/(3.97ms + 3517·per_dist)。落位后据此选"真实感"工作点重跑 E1/E2 型协同扫描。


#### E0 实测（2026-09-04，512M 全驻留/f=0/AVX2，6 dump 全部逐字节=engref）

| per_dist (ns) | 预测 QPS | 实测 QPS | 偏差 | 等效算力 vs 服务器核 | 画像 |
|---|---|---|---|---|---|
| 0 | （历史 251.7） | 233.4 | 会话方差 | 0.53× | 现状=宿主 C 代码，过高 |
| 250 | 206.1 | 197.4 | -4.2% | 0.44× | 强加速器 |
| 500 | 174.5 | 165.2 | -5.3% | 0.37× | 高端 PNM |
| 1000 | 133.5 | 128.3 | -3.9% | 0.28× | 典型 1-2×ARM 控制器 |
| 2000 | 90.8 | 88.1 | -3.0% | 0.19× | 弱嵌入式核 |

- 旋钮线性可预测（QPS ≈ 1/(3.97ms + 3517·per_dist)，全点偏差 -3~-5%，系统偏移 =
  会话内引擎基线比历史低 ~7% 之故），harmonic 曲线形状精确成立。
- 外推（零干扰 max 模型，512M，CPU 标量 358.7）：v=1000 时 f\*=0.74、协同峰值 ≈487
  QPS = 纯 CPU +36%——**引擎阉到 1/3.5 核，协同增益仍可观**；论文叙事成立："设备
  不需要强算力也有贡献，靠的是近端性与容量"。
- 待选点重跑：E1'（512M 协同 f 扫描）/ E2'（256M，注意引擎 job 变慢会拉长 FTL 线程
  占用，干扰画像需重测）在选定工作点（推荐 v=1000）重跑。


## 8.10 Phase C：21M×768d 真实语料规模验证（2026-09-05 完成 E1'）

### 8.10.1 数据集与建图
- 语料 = HF `kenhktsui/wiki_dpr_e5`：21,015,300 条 Wikipedia 段落 × 真实 e5-base-v2
  (768d) 检索向量（fp32 归一化→fp16，CC-BY-SA-3.0）；查询 = NQ test 前 1000 题 e5
  嵌入；GT = fp16 域暴力 top-10（row0/row999 双重独立抽验一致）。
- 建图 = C++ fp16 原生流式构建器 `tools/build_cyh1.cpp`（hnswlib M=32/efc=200/40 线程，
  2h15m、RSS 峰值 ~41GB，绕开 Python hnswlib ~135GB 内存墙）；blob **37,844.8MB**
  （向量 32.28G + adj0 5.38G + upper 80.3M + levels 21M），avg_deg0=41.5，upper 节点
  656,349，入口 level 5。host 权威副本 `/var/tmp/cylon/anns/index/wiki/`（含参考 dump
  `engref_ef100.dump`），guest 副本 `/var/tmp/anns_wiki/`。host engref recall@10 =
  0.9720（ef=100，11.8ms/query）。
- **HDD 页缓存规则**：机械盘随机 4K 仅 ~11MB/s（99% util），顺序 ~150MB/s。host 侧
  mmap 大 blob 的工具（engref 等）必须先 `cat blob > /dev/null && sync` 顺序预热
  （37.8GB ~1min，engref 冷 >10min → 热 12.7s）。

### 8.10.2 E1'：48GB 设备协同 f 扫描（ef=100/n=1000/k=10/AVX2/-S）
| f | wall(s) | 协同 QPS | T_cpu(s) | T_eng(s) | 引擎 exec/q | misses/q | hops |
|---|---|---|---|---|---|---|---|
| 1.0 标量(含 staging) | ~227 | 4.4 | — | — | — | — | — |
| 0.65 | 142.5 | 7.0 | 142.5 | 78.4 | 224ms | 4322 | 114.9 |
| **0.50** | **114.1** | **8.8** | 111.6 | 114.1 | 228ms | 4294 | 114.6 |
| 0.75 | 163.7 | 6.1 | 163.7 | 58.6 | 234ms | 4326 | 115.0 |
| 0.25 | 159.8 | 6.3 | 56.2 | 159.8 | 213ms | 4291 | 114.4 |
| 0.00 | ~208 | 4.8 | — | ~208 | 208ms | 4300 | 114.4 |
| 1.0 avx | ~217 | 4.6 | — | — | — | — | — |

- **wall(f) = max(f·T_cpu, (1-f)·T_eng) 理想重叠模型逐点精确成立**（114.1/142.5/163.7/
  159.8 vs 预测 114/141/163/160）。
- **f\* = 0.5**：协同 8.8 QPS = 纯 CPU 的 2.0×（wall 114s vs 217-227s，加速 1.9×）。
  21M 规模下 T_cpu ≈ T_eng ≈ 210-230s/千查询——DER 建模的引擎与 guest vCPU 恰好均衡，
  协同余量最大。
- 引擎/query 时间 ≈ misses ~4300 × ~48µs（DER 缺失延迟账单），hops 114.4-115.0 与
  host engref（114.4）一致。
- **验收：7/7 点 dump 与 engref 参考逐字节一致，recall@10 全部 0.9720**——与 Phase A
  （1M SIFT, 0.9896）同等强度。

### 8.10.3 运行时内存墙与部署坑（两次返工换来）
- **运行时 RSS ≈ 设备容量 + guest 16GB DRAM + ~3GB 开销**。96GB 设备 + 16GB = 115GB
  贴 123GB 物理墙 → 页缓存榨干、swap 抖动 → guest 三重故障死机、staging 白跑 2.5h。
  **"容量上限 96GB"≠"运行时上限"**；Phase C 操作点 = **48GB 设备**（`femu-restart.sh
  49152`，几何分支现成），48+16 ≈ 70GB 健康。
- **跨崩溃 blob 损坏陷阱**：scp 返回 ≠ 落盘——宿主崩溃时 guest 页缓存脏页全丢，ext4
  日志只救元数据 → **size 对、md5 错**，skip-push 的 size 检查被骗过。任何推送必须
  host/guest **md5 对账**（e1c_run.sh 已内置门禁：不匹配自动重推+复核）。
- wiki_exp.sh 已加 **44000B dump 尺寸门禁**：客户端暴毙产出 0 字节 dump 时该点判无效
  并中止，不再被管道 `|| true` 静默吞掉。

### 8.10.4 已知问题（不阻塞，待查）
- **collab avx 客户端低频竞态**：每 sweep ~1/7 概率、点位随机，客户端 GPF 野跳转
  （dmesg `general protection fault`，ip 落在指令流中间 = 控制流被踩）。引擎侧无恙
  （把已提交 job 做完即空闲），下一点正常恢复；已完成点不受影响（逐字节门禁保证）。
  处置 = 该点重试即可；根因（邮箱并发路径内存污染）待专项排查。
- verify-window 恒报 `1 pages differ [p9239448]`：queries 区尾页口径差（staging 写满
  页 vs verify 按文件 EOF 截断），搜索实际读的字节正确（全部 dump 逐字节一致），暂不修。

### 8.10.5 待做
- E2'（DER_FLUSH=2 真驱逐 + 小 bufsz）与 E0'（per_dist=1000 重放）在 21M 上重跑；
- per_dist 参数微调（已指示暂不做）；NUMA-node 化路线（dax-kmem）已在可行性层面
  评估，待立项。


## 9. 故障排查

| 症状 | 原因/处理 |
|---|---|
| guest 里碰 CXL 窗口后 SSH 断、QEMU 控制台无响应 | FTL 线程活锁（已修复：`cxlssd_init` 设 `dataplane_started=true`，改动在本地 FEMU 未提交 git）。若仓库重置需重打补丁 |
| staging 中途整机冻结（R-state 进程、NMI 不可达、host vCPU 线程栈空） | vCPU 卡死在 host 用户态 bulk MMIO exit 处理里。**用验收模式**（§8.1 plain RAM memslot）绕开；完整 DER 模式下此路径 Phase B 再治 |
| QEMU 进程直接消失（8080 拒连），log 止于 `search job …` | 同 FEMU 多 run 后随机 SIGSEGV（dmesg: `shr esi,12 … je … mov rax,[rsi]` fault 在 `logical_space+off` 越界）= 旧 slot-alias bug（§8.5，已修）；修复后若再见 `!! graph read past window end` 说明仍有页内容错位，查缓存一致性 |
| 同 FEMU 重跑：staging ~3.2s 且 BIND bad magic（magic 形如 0x47xxxxxx） | 残留 direct EPTE 绕过 FTL。pnm_client 已自动先 FLUSH（§8.5）；若复现查 cylon_cache_reset 路径 |
| recall 在 0.92–0.98 随机漂移，dist/hops 却与 host 一致 | 信箱 pickup 竞态：引擎一次 memcpy 读整个信箱，看到"新 PENDING+旧 a0"，~1% job 跑成上一条查询。已修复（先读 status 再读 job+二次校验）。诊断用 `revq`（§8.3） |
| `nproc` 只有 1 / KVM 报 cpus (1) | L0 进错内核或缺 `X86_X2APIC`（见 §2.1） |
| `cxl create-region` 报 ENXIO | guest 内核缺 `CXL_REGION_INVALIDATION_TEST`/`CXL_MEM_RAW_COMMANDS`（-3 起 deb 已含） |
| devdax mmap EINVAL "vma is not DAX capable" | guest 内核缺 `FS_DAX`（-4 起 deb 已含） |
| setup_cxl.sh 报 libdaxctl/libndctl 缺失 | 用 `sudo env LD_LIBRARY_PATH=/usr/lib /usr/local/bin/setup_cxl.sh …` |
| FEMU/guest 重启后客户端 `open /dev/dax0.0: No such file or directory` | guest 每次重启后需重建 devdax 节点：`sudo env LD_LIBRARY_PATH=/usr/lib /usr/local/bin/setup_cxl.sh devdax`（2026-09-06，D2 验证首跑即中） |
| QEMU 卡死诊断 | QMP socket 是 root-only：`sudo python3 /tmp/qmp_diag.py`（查 vCPU 状态+寄存器）；Ctrl-A C 在主循环持 BQL 时无响应 |
| FEMU 端口/进程 | `pgrep -af qemu-system-x86 | grep -v grep`（pgrep -f 带子串会自匹配） |

## 10. 与论文 eval 的对应

artifact（github.com/MoatLab/Cylon）的 `Cylon-scripts/` 提供 MLC/Mio/Redis/GAPBS
复现脚本与画图；`tools/setup_dev.sh` 即本机 `setup_cxl.sh` 的原型。
论文流程 = system-ram 模式 + numactl 绑 CXL 节点跑负载。
CCA 控制面（ivshmem，PCI 00:04.0，guest 可 mmap resource2）目前是桩实现，
可扩展为"近存计算"任务环（见 CYLON.md §3.6）。
