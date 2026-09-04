# FEMU-Cylon CXL-SSD 使用文档（cylon-v9.0.1 分支）

> 本文档基于对 `cylon-v9.0.1` 分支源码的完整阅读与在本机的实际编译/运行尝试整理而成，
> 覆盖：架构原理、代码地图、编译步骤、运行前置条件、运行脚本逐段解读、Guest 内使用方法、
> 参数速查、已知问题与排错。
>
> 一句话概括：**这是在上游 FEMU 9.0（基于 QEMU 9.0.1）之上加入的 "CXL-SSD" 模拟模式
> （`femu_mode=6`）——把 SSD 的存储空间以 CXL Type-3 内存设备的形态（CXL.mem 内存语义）
> 暴露给 Guest，由设备内 FTL + DRAM 缓存 + NAND 延迟模型来承担"SSD"的行为，
> 并通过打补丁的宿主内核 KVM 实现"缓存命中页 EPT 直通、未命中页 MMIO 陷入"的双模访存。**

---

## 1. 这是什么

上游 FEMU 是 QEMU/KVM 之上的 NVMe SSD 模拟器，把模拟 SSD 作为 **NVMe 块设备**（`/dev/nvme0n1`）
呈现给 Guest。本分支（Cylon）在此基础上新增了第 6 种模式 `FEMU_CXLSSD_MODE`：

| femu_mode | 模式 | 说明 |
|---|---|---|
| 0 | OCSSD | Open-Channel 白盒 SSD |
| 1 | BBSSD | 黑盒 SSD（设备管 FTL） |
| 2 | NOSSD | 尽快 SSD（SCM 级延迟） |
| 3 | ZNSSD | Zoned Namespace |
| 4/5 | SMART / KV | （预留/其他） |
| **6** | **CXLSSD** | **Cylon CXL-SSD（本分支新增）** |

CXL-SSD 与普通 NVMe 模拟的本质区别：

- 存储空间通过 **CXL Type-3（内存扩展设备）** 挂在 CXL 固定内存窗口（`cxl-fmw.0`）上，
  Guest 用 **load/store 内存语义**直接访问它（类似访问一块"挂在 CXL 上的 SSD"），
  而不是走 NVMe 队列协议。
- 每个被访问的 4KB 页有两条路径：
  - **命中设备内 DRAM 缓存** → 通过打补丁的 KVM 直通（EPT 直映到宿主物理页，原生访存速度）；
  - **未命中** → 退化为 MMIO 陷入进 QEMU，由 FTL 线程走"NAND 读/写延迟 + 缓存填充"流程。
  - 这就是 "DER"（Dual-mode/直通-陷入双模）的含义，也是本分支对宿主内核提出补丁要求的原因。
- 同时 **NVMe 路径仍然并存**：`-device femu` 本身还是一块 NVMe 控制器，Guest 里既可以看到
  `/dev/nvme0n1`（走传统 FTL），又可以把同一块 SSD 的空间当 CXL 内存用。

---

## 2. 整体架构

```
+------------------------------------------------ Guest VM (Ubuntu 22.04) -------------------+
|  应用 / cca-lib (Cylon Caching API)                                                        |
|     |  mmap(/dev/dax...)  load/store                                                       |
|     |  ivshmem BAR (控制环: PIN/UNPIN/CACHE_ENABLE ...)                                     |
+-----v----------------------------------------^---------------------------------------------+
      | CXL.mem (load/store)                        | ivshmem 共享内存 (/dev/shm/ivshmem0)
      v                                             |
+------------------------------------- QEMU (x86_64-softmmu, FEMU-Cylon) ---------------------+
|  CXL 固定内存窗口 cfmws (dual-mode MR, base_gpa 由 pc.c 记录)                                |
|     | cxl_type3_read/write (hw/mem/cxl_type3.c, femu= 链接)                                 |
|     v                                                                                       |
|  FemuCtrl::cxl_mem_ops.read/write  (hw/femu/cylon/cxlssd.c)                                 |
|     |  cxl_skip_ftl=1: 直接 memcpy 后端                                                     |
|     |  cxl_skip_ftl=0: wait_for_buf_update() -> cxl_req ring -> pqueue(按到期时间排序)       |
|     v                                                                                       |
|  ftl_thread_cxlssd (hw/femu/cylon/ftl_thread.c) —— 单 FTL 线程                               |
|     |  CCA 控制环处理 | CXL 请求处理(缓存查/填 + NAND 延迟) | NVMe 请求处理(传统 FTL)          |
|     v                                                                                       |
|  共享 FTL (hw/femu/ftl/ftl.c, 由 bbssd/ftl.c 迁移扩展) + 缓存插件 (hw/femu/cylon/cache/)      |
|     - 替换策略插件: LIFO / FIFO / CLOCK / S3FIFO                                             |
|     - 缓存槽位缓冲: mmap("/dev/cmahog") 4KB 槽 x N (声明 hpa_base=0xae80000000)              |
|     |                                                                                       |
|     +-- der_kvm.c: 把 NAND 后端注册为 KVM memslot(0x2A) + 逐页改 EPTE                          |
|            命中页 -> EPT 直映缓存槽 HPA (DIRECT_MASK)                                        |
|            未命中页 -> MMIO 陷入 (MMIO_MASK)                                                 |
+----------------------------------^----------------------------------------------------------+
                                   | KVM ioctl: KVM_SET_USER_MEMORY_REGION / KVM_GET_LINEAR_EPT(0xde)
                                   | KVM_SET_EPTE_FLAG(0xdd)      *** 需要打补丁的宿主内核 ***
+----------------------------------v------------------+
                宿主机 (x86_64, KVM, CMA 设备 /dev/cmahog, 补丁内核)
```

Guest 视角同时存在：

1. **CXL 内存窗口**（`cxl-fmw.0`，大小 = SSD 容量）：Guest 内用 `ndctl`/`cxl` 工具创建 region，
   得到 `/dev/dax*` 或 `/dev/pmem*`，mmap 后即可以内存语义访问"SSD"。
2. **NVMe 控制器**（`-device femu`）：`/dev/nvme0n1` 仍可用，走同一 FTL 的传统 NVMe 路径
   （写 NVMe 会使缓存中对应页失效/回写，见 `cache_flush_page`）。
3. **ivshmem 共享内存**（1MB `/dev/shm/ivshmem0`）：宿主 QEMU 与 Guest 用户态 cca-lib 之间的
   CCA 控制环（`cca_shmem.h` 定义布局，注释声明需与外部 `cca-lib/include/cca_layout.h` 一致）。

---

## 3. 关键数据流

### 3.1 CPU 访问路径（CXL.mem load/store）

```
Guest ld/st (CXL 窗口 GPA)
  -> 宿主 EPT: 命中页 => 直映宿主物理页(原生速度, 不经过 QEMU)
             : 未命中页 => EPT 为 MMIO 属性 -> 陷入 KVM -> 转入 QEMU
  -> pc.c 注册的 dual-mode CFMWS MR -> cxl_type3_read/write() -> femu->cxl_mem_ops.read/write()
  -> cxlssd.c: wait_for_buf_update():
        构造 cxl_req{addr, size, data_ptr, is_read} 入 cxl_req 环
        自旋消费 cxl_resp 环 + pqueue(按 expire_time)，直到本请求到期
        (expire_time = start + NAND 延迟，由 FTL 线程计算)
  -> ftl_thread_cxlssd() 处理:
        lpn = addr >> 12 (4KB 页)
        缓存 lookup 命中: 置 dirty/重新插入(可触发预取)
        未命中: 查映射表
            已映射 -> NAND_READ (ssd_advance_status 计算延迟)
            未映射 -> 分配新页 get_new_page + maptbl/rmap/valid + 写指针推进, NAND_WRITE
            creq->expire_time += lat; 缓存 entry_init + insert(INSERT_PREFETCH)
        数据搬运: 缓存槽 <=> data_ptr (memcpy, 槽大小 CACHE_PAGE_SIZE=4096)
        应答入 cxl_resp 环; should_gc() 时 do_gc()
```

### 3.2 DMA 路径

`system/physmem.c` 对 `mr->dual_mode` 的 region 做特判：

- `address_space_map()`：通过 `femu_get_backend_ptr_from_gpa()`（dram.c，GPA→宿主指针）
  直接拿到后端地址，并逐页调用 `cxl_mem_ops.read(..., MEMTXATTRS_DUAL_MODE_DMA_MAP)`
  "touch"一遍 —— 供 DER-KVM 把该范围 EPTE 置 direct（DMA 也走快路径）。
- `address_space_unmap()`：逐页调用 `cxl_mem_ops.write(..., DUAL_MODE_DMA_MAP)` 触发回写语义。

### 3.3 DER-KVM（hw/femu/cylon/der_kvm.c）——依赖补丁内核的部分

- 把整块 NAND 后端（`mbe->logical_space`，即整个 SSD 容量）注册为
  **KVM memslot（slot 0x2A，guest_phys_addr = CXL 窗口 base_gpa）**，
  flag 用了自定义的 `KVM_MEMSLOT_DUAL_MODE (1UL<<17)`。
- `KVM_GET_LINEAR_EPT (0xde)`：一次性取得该 memslot 的线性 EPT 叶子表（QEMU 侧 mmap 的页表
  缓冲），此后 QEMU 可 O(1) 改写每页 EPTE。
- 缓存替换时：`der_kvm_epte_set_trap()` 把被驱逐页 EPTE 置 MMIO 陷入；
  缓存填充时：`der_kvm_epte_set_driect(lpn, hpa)`（注意函数名拼写就是 `driect`）
  把页 EPTE 置为 `缓存槽 HPA | DIRECT_MASK`。
- 这三类操作（自定义 ioctl、自定义 memslot flag、EPTE 语义 flag `0x600000000000977`/`0x586`）
  **全部依赖 Cylon 打过补丁的宿主 KVM**。上游 Linux 内核头文件 `/usr/include/linux/kvm.h`
  中没有 0xdd/0xde 这两个 ioctl（已实测确认），未打补丁的内核上会在设备初始化时
  `perror` 后直接 `abort()`（cxlssd.c: `der_kvm_set_user_memory_region() != 0 -> abort()`）。

### 3.4 设备内 DRAM 缓存（hw/femu/cylon/cache/）

- `cache_backend.h`：缓存槽位缓冲区。若设置了 `cache_backend_dev`（`/dev/cmahog`）则
  mmap 该设备（+`mlock`），槽 hpa_base 取 `cache_hpa_base` 属性（脚本硬编码 `0xae80000000`，
  必须与该 CMA 区域真实物理地址一致，否则 EPT 直映会指向错误物理页）；若未设置则回退使用
  NAND 后端前 bufsz 字节（见 §9 已知问题）。
- `cache.h`：`CacheSet`（分 set，way 可配 1/2/4/8/16/FULL，由 `buffer_way` 决定）、
  `CacheEntry{lpn, dirty, slot_id, ...}`、统计。
- `policy/`：策略插件注册表 + 四种实现（lifo.c / fifo.c / clock.c / s3fifo.c），
  ID 由 femu.h 枚举决定：`NONE=0, LIFO=1, FIFO=2, CLOCK=3, S3FIFO=4`。
  策略只负责选 victim/维护元数据；数据拷贝与 EPTE 切换统一在 cache.c 的
  "evict(epte_set_trap+回写) -> memcpy -> epte_set_direct -> policy insert" 流程里。
- `prf_dg`（`prefetch_degree`）：miss 插入时顺带预取后续 N 页（INSERT_PREFETCH 路径）。

### 3.5 NVMe 路径（并存）

`cxlssd_io_cmd` 对 READ/WRITE 直接转 `nvme_rw`，与 BBSSD 相同地走
`to_ftl/to_poller` 环，由同一个 `ftl_thread_cxlssd` 的 NVMe 分支调用共享 FTL 的
`ssd_read/ssd_write`。因此 Guest 里 `/dev/nvme0n1` 可正常 fio。NVMe 写会通过
`cache_flush_page()` 保持与 CXL 缓存视图的一致。

### 3.6 CCA（Cylon Caching API，ivshmem 控制面）

- QEMU 启动时若设置了 `cca_dev`（指向 ivshmem 的 memory-backend），`cxlssd_init` 会在共享
  内存里布置：头部（magic `0x43434131`，version 1）+ 两条控制环（guest→FEMU / FEMU→guest，
  各 2048 项）+ 槽池（`cca_ctrl_slot_s{cmd,resp}`）。
- Guest 用户态库（外部仓库 cca-lib，布局需与 `cca_shmem.h` 匹配）投递命令：
  `NOP / CACHE_ENABLE / CACHE_DISABLE / PIN / UNPIN / INVALIDATE`（LPN 区间粒度）。
- **FEMU 侧目前对这些命令是桩实现**（`handle_cca_ctrl_cmd` 直接回 status=0），
  即控制面协议已通、语义未实现，做实验时不要指望 PIN/UNPIN 影响实际缓存行为。

---

## 4. 代码地图（相对上游 FEMU 9.0 的改动，`git diff dad43f010..cylon-v9.0.1`）

| 路径 | 作用 |
|---|---|
| `hw/femu/cylon/cxlssd.c/.h` | CXL-SSD 设备注册、CXL.mem read/write 入口、CCA ivshmem 布局、NVMe 命令转发 |
| `hw/femu/cylon/ftl_thread.c` | CXL-SSD 的 FTL 线程：CCA 控制环 + CXL 请求 + NVMe 请求三合一处理循环 |
| `hw/femu/cylon/der_kvm.c/.h` | DER-KVM：KVM memslot 注册、线性 EPT 获取、逐页 EPTE trap/direct（依赖补丁内核） |
| `hw/femu/cylon/cache/` | 设备内 DRAM 缓存框架：cache.c（核心流程）+ policy/（LIFO/FIFO/CLOCK/S3FIFO 插件） |
| `hw/femu/cylon/cca_shmem.h` | CCA ivshmem 共享内存布局（与外部 cca-lib 对应） |
| `hw/femu/ftl/{ftl.c,ftl.h}` | 从 `bbssd/` 迁移上来的共享 FTL，新增缓存插件挂钩、按模式启动不同 FTL 线程 |
| `hw/femu/backend/dram.c` | NAND 后端分配改为页对齐；新增 `femu_set/get_base_gpa()`、`femu_get_backend_ptr_from_gpa()`（GPA→宿主指针） |
| `hw/i386/pc.c` | CXL 固定内存窗口初始化时改用 `memory_region_init_io_dual_mode()` 并 `femu_set_base_gpa(fw->base)` |
| `system/memory.c` + `include/exec/memory.h` | 新增 dual-mode MemoryRegion 初始化接口 |
| `system/physmem.c` | `address_space_map/unmap` 的 dual-mode 特判（DMA 快路径） |
| `include/exec/memattrs.h` | 新增 `dual_mode_dma` 事务属性 |
| `hw/mem/cxl_type3.c` | CXL Type-3 设备新增 `femu=` 属性（`DEFINE_PROP_LINK` 到 FemuCtrl），读/写回调转 `cxl_mem_ops`；CDAT/DVSEC 按 femu 容量生成 |
| `hw/femu/nvme-io.c` | NVMe 命令拷贝改 AVX512/AVX/SSE2 分档；`multipoller_enabled` 分档（每队列独立 poller） |
| `hw/femu/bbssd/ftl_thread.c` | BBSSD 专用 FTL 线程（多 poller 版），与 cylon 版并存 |
| `include/hw/femu/femu.h` | 原 `hw/femu/nvme.h` 迁移/扩充：FemuCtrl 增加 cxl_mr/cxl_as/cxl_mem_ops/cxl_req/rep/bufsz/cca_dev/base_gpa 等字段；femu_mode 枚举加 CXLSSD |
| `femu-scripts/run-cxlssd.sh` | CXL-SSD 启动脚本（详见 §7） |
| `hw/femu/backend/dram copy.c` | 疑似误提交的备份文件（无害但建议清理） |

---

## 5. 编译

### 5.1 硬性前提：x86_64 宿主机

FEMU 自身代码无条件使用 x86 SIMD 内建（`hw/femu/inc/rte_ring.h` 引 `<xmmintrin.h>`，
`nvme-io.c` 用 AVX512/AVX/SSE2 内建），**在 aarch64 主机上连编译都无法通过**。本机
（鲲鹏 TaiShan-v110, aarch64, Ubuntu 25.04, gcc 14.2）实测报错：

```
/mnt/.../hw/femu/inc/rte_ring.h:98:10: fatal error: xmmintrin.h: No such file or directory
ninja: build stopped: subcommand failed.
```

上游 FEMU 官方也只支持 x86-64（README: Platform x86-64）。ARM 主机请直接换机器。

### 5.2 标准编译步骤（x86_64, Debian/Ubuntu）

```bash
cd femu
mkdir build-femu && cd build-femu
cp ../femu-scripts/femu-copy-scripts.sh .
./femu-copy-scripts.sh .
sudo ./pkgdep.sh          # gcc pkg-config git libglib2.0-dev libfdt-dev libpixman-1-dev
                          # zlib1g-dev libaio-dev libslirp-dev libnuma-dev ninja-build
./femu-compile.sh         # ../configure --enable-kvm --target-list=x86_64-softmmu
                          #            --enable-slirp --disable-docs --extra-cflags=-Wno-vla
                          # && make -j$(nproc)
```

产物：`build-femu/x86_64-softmmu/qemu-system-x86_64`。

注意：

- 本仓库 QEMU 基线是 9.0.1。`femu-compile.sh` **没有**传 `--disable-werror`（脚本里该参数
  被注释掉了）。在比官方测试环境更新的 gcc（如 gcc 13/14）上，若遇到 -Werror 报错，
  自行改为 `--disable-werror` 再编译即可（属于正常的 QEMU 折旧问题，不是 Cylon 的问题）。
- `--enable-slirp` 需要 `libslirp-dev`（pkgdep.sh 已包含）；漏装时 configure 会直接报错。
- 编译不依赖任何 Cylon 外部组件（补丁内核 / cmahog 只影响运行期），所以"能否编译通过"
  可以在任意 x86 机器上先行验证。

---

## 6. 运行前置条件清单（全部满足才能跑通）

按脚本 `run-cxlssd.sh` 逐项核对：

1. **x86_64 物理机 + KVM**（README 明确不建议嵌套虚拟化）。`/dev/kvm` 可用，且建议把用户
   加进 `kvm` 组（脚本本身用 sudo 启 QEMU，可绕过组要求）。
2. **打过 Cylon 补丁的宿主内核（CylonLinux）**：**已随 artifact 公开** ——
   [MoatLab/Cylon](https://github.com/MoatLab/Cylon) 仓库里的 `CylonLinux/` 是完整的
   Linux **6.4.6** 内核树（已实锤含 DER 补丁：`include/linux/kvm_host.h:61`
   `KVM_MEMSLOT_DUAL_MODE (1UL<<17)`，`arch/x86/kvm/x86.c` 引用），按普通内核构建安装即可。
   需要内核侧支持：`KVM_GET_LINEAR_EPT (0xde)`、`KVM_SET_EPTE_FLAG (0xdd)`、
   `KVM_MEMSLOT_DUAL_MODE` memslot flag、以及 `DIRECT_MASK/MMIO_MASK` EPTE 语义
   （ioctl 分发不在 UAPI 头里，但 QEMU 侧 `der_kvm.c` 与该内核树配套，直接配对使用即可）。
   **原版内核必然在启动时 abort**（见 §11）。
3. **缓存槽后端：不需要 `/dev/cmahog` 模块**。artifact 的官方路径是用 `memmap` GRUB
   参数在宿主保留一块连续物理内存（表现为 `/dev/pmem*`，见
   `docs/backend-memory-setup.md`）：
   - `lsmem --output-all`（或 `/proc/iomem` + node meminfo）确定目标 NUMA 节点的物理范围；
   - 在 `/etc/default/grub` 加 `memmap=<size>!<start>`（选节点尾部，如 `memmap=64G!448G`），
     `update-grub` + 重启，验证 `ls /dev/pmem*`；
   - 把 `run-cxlssd.sh` 头部三个参数改为：`cache_backend_dev="/dev/pmem0"`、
     `cache_hpa_base=<保留区物理基址>`（替换硬编码 `0xae80000000`）。
   保留区大小：本仓库脚本只用它放 512MB 缓存槽（≥512MB 即可）；artifact 脚本则把
   NAND 后端也放进去（`backend_dev`，需 ≥SSD 容量，文档示例 64G）。
4. **Guest 镜像**：`~/images/ubuntu22.qcow2`（脚本硬编码）。**Guest 侧完全 unmodified**
   （论文口径）：Guest 里看到的是标准 CXL 2.0 Type-3 设备（标准 DVSEC/CDAT + CXL.mem，
   `cxl_type3.c` 中 `femu=` 只是把背板 MR 换成 FEMU 的 `cxl_mr`，仍按 volatile（VMEM）
   语义暴露），可呈现为 DAX region 或 CPU-less NUMA node。Guest 只需内核带 CXL 支持
   （论文评价用 Ubuntu 22.04 + vanilla v6.4.6，无任何 guest 侧补丁；Ubuntu 22.04 GA 5.15
   的 CXL 支持偏老，建议 HWE 6.x 或自编译 vanilla）+ ndctl/cxl 用户态工具。
   补丁内核只在宿主 KVM 侧（见第 2 条）。实验 CCA 还需在 Guest 里装外部 `cca-lib`
   （用户态 mmap ivshmem BAR，同样零内核改动）。
5. **资源核算**（以默认 48GB 档为例）：
   - NAND 后端 = SSD 容量（48GB）的宿主内存（FEMU 一贯的 DRAM-backed 实现）；
   - Guest 16GB（脚本 `dram_size=16G`，`policy=bind` 绑到 **node0** 并 `prealloc=on`，
     要求 node0 空闲 ≥16GB，否则启动即失败；如需换节点改脚本 `host-nodes=`）；
   - 缓存槽 512MB（memmap 保留区 `/dev/pmem0`，替代 cmahog）+ ivshmem 1MB；
   - 8 vCPU（`n_threads=8`）。96GB 档则要求宿主空闲内存 ≥ 96+16+GB。
6. 脚本会做两个系统级调整（需要 root）：关 `numa_balancing`、THP 设为 `never`。

---

## 7. run-cxlssd.sh 逐段解读

```
./run-cxlssd.sh [ssd_size_MB]
```

- **仅支持两个取值**：`49152`（48GB）或 `98304`（96GB）。传其它值时几何参数变量为空，
  生成的命令行直接非法。（几何：secsz=512B，8 sect/pg，256 pg/blk，8 LUN/通道 × 8 通道，
  48GB=768 blk/PL、96GB=1536 blk/PL，不支持多 plane。）
- ivshmem：不存在则 `dd` 生成 1MB `/dev/shm/ivshmem0` 并 `chmod 666`。
- NAND 延迟：页读 40µs / 页写 200µs / 块擦 2ms / 通道传输 0。
- GC 阈值：75% / 95%。
- 缓存：`bufsz=512MB`，`policy=2`（**代码语义是 FIFO**，脚本注释把 3/4 标反了，见 §9）、
  `prefetch_degree=0`、`skip_ftl=0`。
- QEMU 侧关键参数：
  - `q35,accel=kvm,cxl=on,nvdimm=on`，`-cpu host`，8 vCPU，16GB 内存绑 node0；
  - `-device femu,id=femu-cxlssd` + 一长串 `-device` 属性（femu_mode=6、几何、延迟、缓存等，
    全表见 §8）；
  - CXL 拓扑：`pxb-cxl`(bus_nr=12) → `cxl-rp` → `cxl-type3,femu=femu-cxlssd`，
    以及 `-M cxl-fmw.0.targets.0=cxl.1,cxl-fmw.0.size=${ssd_size}M`（内存窗口大小=SSD 容量）；
  - `-device ivshmem-plain` + `memory-backend-file(/dev/shm/ivshmem0)`：CCA 控制面；
  - 网络用户态 NAT，宿主 8080 → Guest 22（`ssh -p8080 user@localhost`）；
  - `-nographic`（串口控制台，要求镜像已配好 ttyS0），QMP socket `./qmp-sock`，
    全部输出 `tee log`。

---

## 8. `-device femu` 属性速查（CXL-SSD 相关）

| 属性 | 默认 | 说明 |
|---|---|---|
| `femu_mode` | 2 (NOSSD) | **必须显式传 6** 才是 CXL-SSD |
| `devsz_mb` | 1024 | SSD 容量（MB），同时决定 NAND 后端内存 |
| `cxl_skip_ftl` | 0 | 1 = CXL.mem 访问绕过 FTL/延迟，直接 memcpy（纯内存语义对照组） |
| `secsz` | 512 | 扇区大小 |
| `secs_per_pg` | 8 | 每页扇区数（→4KB 页） |
| `pgs_per_blk` | 256 | 每块页数 |
| `blks_per_pl` | — | 每 plane 块数（几何因容量而异） |
| `pls_per_lun` | 1 | 多 plane 不支持 |
| `luns_per_ch` / `nchs` | — | 8 / 8 |
| `pg_rd_lat` / `pg_wr_lat` / `blk_er_lat` / `ch_xfer_lat` | ns | NAND 延迟 |
| `gc_thres_pcent` / `..._high` | 75 / 95 | GC 触发/强制阈值 |
| `bufsz_mb` | 0 | 设备内 DRAM 缓存容量（0 = 关闭缓存插件） |
| `replacement` | 1 (LIFO) | 策略 ID：0=NONE 1=LIFO **2=FIFO 3=CLOCK 4=S3FIFO**（按 femu.h 枚举） |
| `prefetch_degree` | 0 | miss 时顺带预取的后续页数 |
| `buffer_way` | 0 | 缓存组相联度 0/1/2/3/4/5 → 1/2/4/8/16/FULL-way |
| `cache_backend_dev` | 无 | 缓存槽位后端设备（/dev/cmahog） |
| `cache_bdev_offset` | 0 | cmahog 内偏移 |
| `cache_hpa_base` | 0 | 缓存槽 HPA 基址（须与 CMA 真实物理地址一致） |
| `backend_dev` / `bdev_offset` / `hpa_base` | 无 | NAND 后端本体也可挂设备（脚本未用） |
| `cca_dev` | 无 | CCA ivshmem memory-backend 链接 |
| `multipoller_enabled` | 0 | 1 = 每个队列独立 poller 线程 |
| `serial` / `namespaces` / `queues` / ... | 同上游 | NVMe 控制器属性 |

运行期控制（Guest 内经 NVMe admin passthru，opcode `0xef` = `NVME_ADM_CMD_FEMU_FLIP`，
cdw10 选择子）：

```
FEMU_ENABLE_GC_DELAY / FEMU_DISABLE_GC_DELAY     # 开关 GC 延迟
FEMU_ENABLE_DELAY_EMU / FEMU_DISABLE_DELAY_EMU   # 开关 NAND 延迟模拟(用编译期常量)
FEMU_RESET_ACCT                                  # 清零 I/O 统计
FEMU_ENABLE_LOG / FEMU_DISABLE_LOG               # 开关 QEMU 侧日志
```

---

## 9. 已知问题与不一致（阅读源码 + 本机实测发现）

1. **x86-only**（实测）：aarch64 上编译失败于 `rte_ring.h:98 xmmintrin.h`（§5.1）。
2. **外部依赖**：仓库本体不含补丁内核/缓存后端/镜像，但 **artifact（[MoatLab/Cylon](https://github.com/MoatLab/Cylon)）已公开全部配套**：`CylonLinux/`（补丁内核全树）、`docs/`（backend-memory-setup、guest-image-setup）、`tools/`（setup_dev.sh、cxl_warmup.c、pin_binary、compile.sh）、`Cylon-scripts/`（MLC/Mio/Redis/GAPBS 复现脚本+数据+画图）。只有 Guest 镜像和 cca-lib 需要自备/向作者获取。
3. **无补丁内核时必然 abort**：`cxlssd_init()` 中 `der_kvm_set_user_memory_region()` 失败
   直接 `abort()`（原版内核上 `KVM_GET_LINEAR_EPT` 为未知 ioctl；且 memslot flag
   `1<<17` 会被原版 KVM 拒绝）。没有降级路径。
4. **脚本注释与代码相反**：`run-cxlssd.sh` 写 `policy=2 # [1:LIFO 2:FIFO 3:S3FIFO 4:CLOCK]`，
   而 `femu.h`/`cache_policy.h` 的枚举是 `1:LIFO 2:FIFO 3:CLOCK 4:S3FIFO`（3/4 对调）。
   默认值 2=FIFO 恰好一致，但如果按注释选 3/4 就会拿错策略。
5. **`/dev/cmahog` 缺失时的回退语义可疑**：`cylon_cache_backend_init()` 在没有
   `cache_backend_dev` 时把 `buf_space` 指到 NAND 后端起始处（缓存槽与"闪存"数据重叠），
   且 `hpa_base=0`；EPT 直映路径会失效。实验时建议始终显式提供 cmahog 或干脆
   `bufsz_mb=0` 关闭缓存。
6. **`cache_hpa_base` 硬编码**：`0xae80000000` 是作者机器上 CMA 区域的物理地址，换机器必须改，
   且脚本不会校验。
7. **小问题**：`hw/femu/backend/dram copy.c` 是误提交的备份文件；`der_kvm_epte_set_driect`
   函数名拼写错误（driect→direct）；`include/hw/femu/femu.h` 中 CXL 请求默认
   `lba_index=3`（4096B LBA）与上游默认 0（512B）不同，属有意改动但未见文档说明。
8. **CCA 控制命令为桩**：PIN/UNPIN/INVALIDATE 只回 success，不改变设备行为（§3.6）。

---

## 10. Guest 内的典型用法

```bash
# 1) 确认 CXL 内存设备（内核 >= 5.x 且带 CXL 驱动）
cxl list
ndctl list -R

# 2) 创建 region 并拿到 DAX 设备（QEMU 模拟的是 volatile（vmem 语义）的 Type-3）
cxl create-region -m -d <decoder> -w 1     # 或用 ndctl；得到 /dev/dax0.0

# 3) 挂上用（两种模式）
daxctl reconfigure-device --mode=devdax /dev/dax0.0    # 设备DAX: 应用直接 mmap, load/store 即访问SSD
daxctl reconfigure-device --mode=system-ram /dev/dax0.0 # 当普通内存用(配合 numactl 指定节点)
# 或 fsdax 后 mkfs 挂载

# 4) NVMe 视角（同一块 SSD 的另一入口）
nvme list; fio --filename=/dev/nvme0n1 ...

# 5) FEMU 运行期开关（示例）
nvme admin-passthru /dev/nvme0 --opcode=0xef --cdw10=0x100000003   # 具体选择子见 femu.h
```

宿主侧观察 QEMU 日志：`build-femu/log`（脚本 `tee log`），Cylon 初始化会打印
`Cylon CXL-SSD init: ...`、`Cylon DER-KVM: ...`、`Cylon cache backend: ...` 等行，
是判断各组件是否成功初始化的最直接证据。

---

## 11. 故障排查速查

| 现象 | 根因 | 处置 |
|---|---|---|
| 编译报 `xmmintrin.h: No such file` | 在 ARM 主机上编 | 换 x86_64 主机（无解） |
| configure 报 slirp 相关错误 | 没装 `libslirp-dev` | `sudo ./pkgdep.sh` |
| gcc 13/14 一堆 -Werror 编译失败 | QEMU 9.0 对新 gcc 的折旧 | configure 加 `--disable-werror` |
| `VM disk image couldn't be found` | 缺 `~/images/ubuntu22.qcow2` | 自建镜像或向上游作者索取 |
| 启动即 `Cylon DER-KVM: KVM_SET_USER_MEMORY_REGION failed: Invalid argument` 或 `KVM_GET_LINEAR_EPT: Inappropriate ioctl` 后 abort | 宿主内核未打 Cylon KVM 补丁 | 必须换补丁内核 |
| `Failed to open cache backend device /dev/cmahog` | CMA 模块未加载/设备不存在 | 装配套内核模块；或 `bufsz_mb=0` 关缓存（会失去直通快路径意义） |
| Guest 读 CXL 区数据错乱 | `cache_hpa_base` 与 cmahog 实际物理地址不符 | 修正为真实 CMA HPA |
| `policy=bind` prealloc 失败 | node0 空闲内存不足 16GB | 改脚本 `host-nodes=` 或腾内存 |
| 传了 48GB/96GB 以外的 ssd_size | 脚本几何参数未定义 | 只用 `49152` / `98304` |
| Guest 内看不到 CXL 设备 | 内核太老 / 未装 ndctl、cxl 工具 | 用带 CXL 支持的 22.04 内核 |

---

## 12. 本机（x86_64）结论（2026-09-01 更新）

当前机器已验证：x86_64，40 核，125GB 内存（可用 ~112GB），`/dev/kvm` 可用且用户在 kvm 组。
**48GB 档完全可行**（需 ~65GB 空闲）；96GB 档偏紧（~113GB），不推荐。

剩余准备工作（按依赖顺序）：

1. 编译 FEMU（不依赖外部组件，可立即做）；
2. 构建并安装宿主补丁内核 CylonLinux（artifact 里的 6.4.6 全树，含 DER 补丁），
   GRUB 加 `memmap=<size>!<start>` 保留连续物理内存 → `/dev/pmem0`；
3. 改 `run-cxlssd.sh`：`cache_backend_dev=/dev/pmem0`、`cache_hpa_base=<保留区物理基址>`
   （替换硬编码 `0xae80000000`）；
4. 构建 Guest 镜像 `~/images/ubuntu22.qcow2`（jammy cloud image + vanilla 6.4.6
   CXL 内核 + ndctl v77，按 artifact `docs/guest-image-setup.md`，可脚本化）；
5. 克隆 artifact 拿 `tools/`（setup_dev.sh 等）和 `Cylon-scripts/`（复现脚本）。

参考资源：
- 论文：[Cylon (FAST'26)](https://huaicheng.github.io/p/fast26-cylon.pdf)
- Artifact: [MoatLab/Cylon](https://github.com/MoatLab/Cylon)
  （CylonFEMU / CylonLinux / Cylon-scripts / docs / tools）
- [Backend memory setup](https://github.com/MoatLab/Cylon/blob/master/docs/backend-memory-setup.md) ·
  [Guest image setup](https://github.com/MoatLab/Cylon/blob/master/docs/guest-image-setup.md)
