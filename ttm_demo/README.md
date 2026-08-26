# TTM 使用过程 Demo

本模块不接 GPU。它搭一个假的 TTM 设备：`SYSTEM` 无限、`VRAM` 只有 16KB，
用来走一遍 amdgpu 里 BO 的核心调用链。

和 `dma_resv_demo` 的关系：`dma_resv` 是每块 buffer 的依赖登记表；
TTM 在 **validate / move / destroy** 前会查这张表，没做完就等待或返回 `-EBUSY`。

## 1. TTM 在驱动里是哪一层

```text
用户 / KFD / GEM ioctl
        │
        ▼
  amdgpu_bo_create / amdgpu_bo_validate / pin
        │
        ▼
  ttm_bo_init_reserved / ttm_bo_validate     ← 本 demo 直接调用
        │
        ├─ resource manager  记账：这块放哪、偏移多少
        │     SYSTEM  : 无限，ttm_sys_man（ttm_device_init 自带）
        │     VRAM    : 有限区间，ttm_range_man / amdgpu VRAM manager
        │     TT/GTT  : 系统页 + GPU 可访问映射（本 demo 未启用）
        │
        ├─ ttm_tt            真正的系统页 backing
        │
        └─ funcs->move       域之间搬数据
              真 GPU : SDMA blit
              本 demo: ttm_bo_move_null() 只换标签
```

## 2. 一次 BO 生命的调用顺序

```text
drm_gem_private_object_init()     填 size，挂上 dma_resv
        │
ttm_bo_init_reserved()            内部立刻 ttm_bo_validate()
        │
        ├─ ttm_resource_alloc()   向目标 domain 要一段空间
        │       不够 → LRU eviction → 再试
        │
        ├─ funcs->move()          NONE/SYSTEM → VRAM
        │       1. ttm_bo_wait_ctx()   等 dma_resv（驱动自己写，核心不代劳）
        │       2. 真 GPU: SDMA blit；本 demo: ttm_bo_move_null()


之后要换地方：reserve → ttm_bo_validate(新 placement) → unreserve
销毁：ttm_bo_put() → 等 fence idle → 释放 resource / ttm → destroy()
```

## 3. 本 demo 跑出来的场景

假 VRAM = 4 页 = 16KB。`ttm_range_manager` 分配的是**连续**页，所以空闲总量够也不一定放得下。

```text
[1] 创建 A=4KB @ SYSTEM，CPU 写入 magic=0xDEADBEEF
      ttm_tt 此时才 populate（kmap 触发）

[2] 给 A 的 dma_resv 挂一个未完成 KERNEL fence
      validate(VRAM, no_wait_gpu=1) → -EBUSY
        真正挡住搬移的是 driver move() 里的 ttm_bo_wait_ctx()
        TTM 核心不会在 validate 入口自动等这块 BO 的 fence
      signal 后再 validate(TOPDOWN) → A 进入 VRAM start=3（顶部）

[3] 创建 B=8KB @ VRAM（从底部）
      布局: [ B 8KB ][ 空 4KB ][ A 4KB ]
      usage = 12KB

[4] 创建 C=8KB @ VRAM
      连续空闲不够 → eviction A (VRAM→SYSTEM)
      布局: [ B 8KB ][ C 8KB ]
      再读 A[0]，magic 仍在：move_null 只改 placement，页没拷贝

[5] pin B 后创建 D=8KB
      只能 eviction 未 pin 的 C；B 留在 VRAM
      布局: [ B 8KB ][ D 8KB ]
```

```text
时间轴上的 VRAM（每格 4KB）:

[1]  ____ ____ ____ ____
[2]  ____ ____ ____ [A ]
[3]  [  B   ]  ____ [A ]
[4]  [  B   ]  [  C   ]      A 被挤到 SYSTEM
[5]  [  B   ]  [  D   ]      C 被挤走，B 因 pin 不动
```

## 4. 和 amdgpu 的对应关系

| Demo | amdgpu |
|---|---|
| `ttm_device_init` | `amdgpu_ttm.c` 里 `ttm_device_init(&adev->mman.bdev, ...)` |
| `ttm_range_man_init(VRAM)` | VRAM / GDS / doorbell 等有限域 |
| `funcs->move` = `wait_ctx` + `move_null` | `amdgpu_bo_move()` → `ttm_bo_wait_ctx` + SDMA `COPY_LINEAR` |
| `evict_flags` → SYSTEM | `amdgpu_evict_flags()` → GTT，再 SYSTEM |
| `bo->base.resv` | 同一块 `dma_resv`，CS / move / CPU 访问都等它 |
| `ttm_bo_pin` | `amdgpu_bo_pin`，scanout / fb / kernel BO 常用 |

TTM 管的是 **placement（这块内存在哪个域、偏移多少）**。
GPU 虚拟地址（GPUVM page table）是更上面一层，本 demo 不涉及。

## 5. 编译与运行

需要能加载 `ttm.ko` 的内核（本机 `6.8` 即可），不需要 AMD GPU。

```bash
cd linux_gpu_driver_demos/ttm_demo
make
make run
```

`dmesg` 里按 `[1]`…`[5]` 看每一步的 `mem=`、`move ... evict=` 和 `VRAM usage=`。
