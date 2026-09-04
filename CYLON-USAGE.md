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

## 9. 故障排查

| 症状 | 原因/处理 |
|---|---|
| guest 里碰 CXL 窗口后 SSH 断、QEMU 控制台无响应 | FTL 线程活锁（已修复：`cxlssd_init` 设 `dataplane_started=true`，改动在本地 FEMU 未提交 git）。若仓库重置需重打补丁 |
| staging 中途整机冻结（R-state 进程、NMI 不可达、host vCPU 线程栈空） | vCPU 卡死在 host 用户态 bulk MMIO exit 处理里。**用验收模式**（§8.1 plain RAM memslot）绕开；完整 DER 模式下此路径 Phase B 再治 |
| QEMU 进程直接消失（8080 拒连），log 止于 `BIND job` | 引擎段错误（曾为：visited 位图用旧 count 分配 → memset(NULL)；已修复挪到 count 赋值后） |
| recall 在 0.92–0.98 随机漂移，dist/hops 却与 host 一致 | 信箱 pickup 竞态：引擎一次 memcpy 读整个信箱，看到"新 PENDING+旧 a0"，~1% job 跑成上一条查询。已修复（先读 status 再读 job+二次校验）。诊断用 `revq`（§8.3） |
| `nproc` 只有 1 / KVM 报 cpus (1) | L0 进错内核或缺 `X86_X2APIC`（见 §2.1） |
| `cxl create-region` 报 ENXIO | guest 内核缺 `CXL_REGION_INVALIDATION_TEST`/`CXL_MEM_RAW_COMMANDS`（-3 起 deb 已含） |
| devdax mmap EINVAL "vma is not DAX capable" | guest 内核缺 `FS_DAX`（-4 起 deb 已含） |
| setup_cxl.sh 报 libdaxctl/libndctl 缺失 | 用 `sudo env LD_LIBRARY_PATH=/usr/lib /usr/local/bin/setup_cxl.sh …` |
| QEMU 卡死诊断 | QMP socket 是 root-only：`sudo python3 /tmp/qmp_diag.py`（查 vCPU 状态+寄存器）；Ctrl-A C 在主循环持 BQL 时无响应 |
| FEMU 端口/进程 | `pgrep -af qemu-system-x86 | grep -v grep`（pgrep -f 带子串会自匹配） |

## 10. 与论文 eval 的对应

artifact（github.com/MoatLab/Cylon）的 `Cylon-scripts/` 提供 MLC/Mio/Redis/GAPBS
复现脚本与画图；`tools/setup_dev.sh` 即本机 `setup_cxl.sh` 的原型。
论文流程 = system-ram 模式 + numactl 绑 CXL 节点跑负载。
CCA 控制面（ivshmem，PCI 00:04.0，guest 可 mmap resource2）目前是桩实现，
可扩展为"近存计算"任务环（见 CYLON.md §3.6）。
