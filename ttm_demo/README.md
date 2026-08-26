# TTM + GEM 教学 Demo

本模块演示 **TTM 如何管理 Buffer Object (BO) 在 SYSTEM 与 VRAM 之间的 placement**。
不接真实 GPU，用 16KB 假 VRAM 把 eviction / move / fence 行为放大到可观察。

## 核心概念：三层结构

```text
┌─────────────────────────────────────────────────────────────┐
│  GEM (drm_gem_object)                                       │
│    size, dma_resv, handle — 用户态看到的“buffer 对象”      │
├─────────────────────────────────────────────────────────────┤
│  TTM (ttm_buffer_object)                                  │
│    resource (placement) — 这块 buffer 放在哪个域、偏移多少  │
│    ttm_tt            — 系统 RAM 页 backing（真正存数据）     │
├─────────────────────────────────────────────────────────────┤
│  Resource Manager                                           │
│    SYSTEM : 无限（ttm_sys_man，device_init 自带）           │
│    VRAM   : 有限（本 demo 仅 4 页 = 16KB）                  │
│    TT/GTT : 系统页 + GPU 映射（本 demo 未启用）             │
└─────────────────────────────────────────────────────────────┘
```

### SYSTEM vs VRAM：数据在哪？

| 域 | 本 demo | 真实 amdgpu |
|---|---|---|
| **SYSTEM** | 只记账；数据在 `ttm_tt` 系统页 | 同上，或 swap 到 shmem |
| **VRAM** | 只改 `resource->mem_type` 标签；数据仍在系统页 | SDMA 把页搬到 GPU 显存 |
| **backing** | `ttm_tt->pages[]` 始终是系统 RAM | VRAM 域也有 `ttm_tt` 做中转 |

关键：**placement（放哪）和 backing（数据在哪）是分开的**。  
本 demo 的 `move()` 调用 `ttm_bo_move_null()`，只换标签不搬数据；amdgpu 会用 SDMA blit。

### GEM 与 TTM 的关系

```text
用户 ioctl (GEM_CREATE / GEM_CLOSE)
        │
        ▼
drm_gem_private_object_init()   ← 设置 size、挂 dma_resv
        │
        ▼
ttm_bo_init_reserved()          ← 立刻 validate 到目标 placement
        │
        ├─ resource manager 分配 resource->start / mem_type
        ├─ funcs->move()     域切换（等 fence → 搬数据或换标签）
        └─ ttm_tt_populate() CPU 访问时填充系统页
```

## 一次 BO 生命周期

```text
create:  ttm_bo_init_reserved(pl=SYSTEM/VRAM)
           └─ 内部 ttm_bo_validate → alloc + move

validate: ttm_bo_validate(pl=新域)
           └─ 空间不够 → LRU eviction → evict_flags → move

destroy: ttm_bo_put()
           └─ 等 dma_resv idle → free resource → destroy ttm_tt
```

## Demo 场景（dmesg 里 [1]–[5]）

假 VRAM = 4 页 = 16KB。`ttm_range_manager` 分配**连续**页，总量够也可能放不下。

```text
[1] A=4KB @ SYSTEM，CPU 写 magic → ttm_tt populate

[2] A 挂未完成 fence → validate(VRAM) + no_wait → -EBUSY
      signal 后成功 → A @ VRAM start=3 (TOPDOWN)

[3] B=8KB @ VRAM  →  [ B 8KB ][ 空 4KB ][ A 4KB ]

[4] C=8KB @ VRAM  →  eviction A→SYSTEM，magic 仍在
      布局: [ B 8KB ][ C 8KB ]

[5] pin B → 创建 D=8KB → 只能踢 C
      布局: [ B 8KB ][ D 8KB ]
```

## 与 amdgpu 对照

| 本 demo | amdgpu (`amdgpu_ttm.c`) |
|---|---|
| `ttm_device_init` | `ttm_device_init(&adev->mman.bdev, ...)` |
| `ttm_range_man_init(VRAM)` | VRAM/GDS/doorbell manager |
| `move` = `wait_ctx` + `move_null` | `amdgpu_bo_move` = wait + SDMA |
| `evict_flags` → SYSTEM | `amdgpu_evict_flags` → GTT → SYSTEM |
| `ttm_bo_pin` | pin / 内核 BO 防 eviction |
| `bo->base.resv` | CS / move / CPU 共用同一张 fence 表 |

TTM 管 **placement**；GPU 虚拟地址（GPUVM）是更上层，本 demo 不涉及。

## 为什么不用真实 VRAM？

1. 本机 amdgpu 已占用所有 GPU VRAM 和 DRM minor 号
2. 假 VRAM 能可控地触发 eviction，便于学习
3. 观察真实 VRAM 请用：`cat /sys/class/drm/card*/device/mem_info_vram_*`

## 编译与运行

内核 6.8（本机 `6.8.0-116-generic`）。旧头 `ttm_bo_api.h` / `ttm_bo_driver.h` 已经合并进 `ttm_bo.h` / `ttm_device.h`。

多卡机器上 `drm_dev_alloc()` 可能因 DRM minor 耗尽返回 `-ENOSPC`，本 demo 改用 `anon_inode` + 手工 `vma_offset_manager`，不注册 `/dev/dri`。

```bash
cd linux_gpu_driver_demos/ttm_demo
make
sudo modprobe ttm
sudo dmesg -C
sudo insmod ttm_demo.ko
sudo rmmod ttm_demo
dmesg | rg ttm_demo
```

或：`make run`

## 建议配合阅读的内核头文件

```text
include/drm/ttm/ttm_bo.h            — BO 生命周期 API
include/drm/ttm/ttm_device.h        — driver 回调、ttm_device
include/drm/ttm/ttm_placement.h     — TTM_PL_* 域定义
include/drm/ttm/ttm_range_manager.h — 有限域区间分配
include/drm/ttm/ttm_tt.h            — 系统页 backing
include/drm/drm_gem.h               — GEM 对象
```
