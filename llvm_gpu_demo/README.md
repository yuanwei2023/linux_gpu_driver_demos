# LLVM → AMDGPU 编译链教学 Demo

用最小 `vec_add` kernel 走一遍：**C 源码 → LLVM IR → GCN 汇编 → ELF object → code object (.co) →（可选）GPU 执行**。

## 编译流水线

```text
kernel.c
   │  clang --target=amdgcn-amd-amdhsa -mcpu=gfx942
   ├─► build/vec_add.ll        LLVM IR（中间表示）
   ├─► build/vec_add.opt.ll    opt 优化后的 IR
   ├─► build/vec_add.s         GCN 汇编（人类可读 ISA）
   ├─► build/vec_add.o         relocatable ELF（单 kernel）
   └─► build/vec_add.co        code object（runtime 加载的“GPU bin”）

host.cpp + vec_add.co
   └─► hipModuleLoadData → hipModuleLaunchKernel → 结果写回 host
```

## 各阶段是什么

| 阶段 | 文件 | 作用 |
|---|---|---|
| **源码** | `kernel.c` | 设备端 C，`__attribute__((amdgpu_kernel))` 标记入口 |
| **LLVM IR** | `vec_add.ll` | SSA 中间表示；opt/后端都从这里继续 |
| **GCN ASM** | `vec_add.s` | 可读 ISA：`s_*` 标量、`v_*` 向量、`global_load/store` |
| **ELF .o** | `vec_add.o` | 带 `.amdhsa_kernel` metadata 的 reloc object |
| **Code object** | `vec_add.co` | `ld.lld -shared` 链接出的 HSA code object |
| **反汇编** | `vec_add.disasm.txt` | 机器码 + 助记符，对照 ISA |

## 快速开始（只编译，不跑 GPU）

```bash
cd linux_gpu_driver_demos/llvm_gpu_demo
chmod +x build.sh
./build.sh
```

或分步：

```bash
make ir      # LLVM IR
make asm     # GCN 汇编
make obj     # .o
make co      # .co
make disasm  # 反汇编
make show    # 列出 build/
```

## 在 GPU 上运行（可选）

需要 ROCm runtime + 可见 GPU（`rocminfo` / `hipinfo`）：

```bash
export LD_LIBRARY_PATH=/opt/rocm/lib:$LD_LIBRARY_PATH
make host run
# 期望输出: result: 11 22 33 44
```

## 读 GCN 汇编：`vec_add.s` 里有什么

典型 MI300/gfx942 kernel 开头：

```text
s_load_dword s2, s[0:1], 0x18     ; 从 kernarg 读 n
v_cmp_gt_i32_e32 vcc, s2, v0      ; v0 = workitem.id.x，边界检查
global_load_dword v1, ...         ; 读 a[i], b[i]
v_add_f32_e32 v1, v1, v2          ; 向量加
global_store_dword ...            ; 写 c[i]
s_endpgm                          ; wavefront 结束
```

- **`s_*`**：scalar unit，每个 wave 一份（队列/地址/控制流）
- **`v_*`**：vector unit，lane 并行（每个 workitem 一份）
- **`global_load/store`**：访问 global memory（对应 `a/b/c` 指针）

## 和 HIP 一键编译的区别

| 手工本 demo | `hipcc` 日常用法 |
|---|---|
| 分步保留 `.ll` / `.s` / `.co` | 一步生成 host + device |
| 用 `hipModuleLoadData(.co)` | 通常 `hipLaunchKernel` 或 graph |
| 适合学 ISA / LLVM 后端 | 适合写应用 |

## 环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `ROCM` | `/opt/rocm` | ROCm 安装根目录 |
| `GPU_TARGET` | `gfx942` | MI300 系列；按实际 GPU 改 |

## 工具路径

本机 ROCm 7.14：

```text
/opt/rocm/llvm/bin/clang
/opt/rocm/llvm/bin/opt
/opt/rocm/llvm/bin/ld.lld
/opt/rocm/llvm/bin/llvm-objdump
/opt/rocm/bin/hipcc
```

注意：ROCm 通常**不单独 ship `llc`**，后端集成在 `clang` 里；`-S` 直接出 GCN 汇编，`-c` 直接出 `.o`。

## 进一步阅读

- `kernel.c` — 最小 device kernel
- `host.cpp` — code object 加载与 launch
- `build/vec_add.s` — 对照 ISA
- `build/vec_add.disasm.txt` — 机器码十六进制

## 如何 debug 执行过程

可以按 **编译链 → host 执行 → GPU kernel** 三层排查。

### 1. 编译链：确认每一步产物

不需要 GPU，先检查中间文件：

```bash
make clean && make ir asm obj co disasm
make show
```

| 怀疑点 | 查看文件 |
|---|---|
| 前端 / IR 不对 | `build/vec_add.ll` — 是否有 `llvm.amdgcn.workitem.id.x` |
| 优化改坏了 | `diff build/vec_add.ll build/vec_add.opt.ll` |
| ISA 不符合预期 | `build/vec_add.s` — `s_load` / `global_load` / `v_add_f32` |
| 机器码 | `build/vec_add.disasm.txt` |
| code object 结构 | `llvm-readobj -h build/vec_add.co` |

```bash
/opt/rocm/llvm/bin/llvm-objdump -d build/vec_add.co
/opt/rocm/llvm/bin/llvm-readobj --symbols build/vec_add.co | rg vec_add
```

确认符号 `vec_add` 存在，否则 `hipModuleGetFunction` 会失败。

### 2. Host 端：debug `run_vec_add`

#### 环境变量

```bash
export LD_LIBRARY_PATH=/opt/rocm/lib:$LD_LIBRARY_PATH
export HIP_VISIBLE_DEVICES=0

export HIP_LAUNCH_BLOCKING=1    # 同步 launch，便于定位 hang
export HSA_ENABLE_DEBUG=1
export HSA_DEBUG=1
```

#### GDB 调试 host

```bash
make host
gdb --args ./build/run_vec_add build/vec_add.co
```

常用断点：

```text
break hipModuleLoadData
break hipModuleGetFunction
break hipModuleLaunchKernel
break hipMemcpy
run
```

重点检查：

- `blob.size()` 是否大于 0
- `hipModuleGetFunction(..., "vec_add")` 是否返回 `hipSuccess`
- `da` / `db` / `dc` / `n` 和 `args[]` 里的地址是否正确

#### 在 host 里加打印（最快）

在 `host.cpp` 关键步骤后加：

```cpp
std::cerr << "mod loaded, launching n=" << n << "\n";
std::cerr << "da=" << da << " db=" << db << " dc=" << dc << "\n";
```

launch 后打印 `hc[]`，期望 `{11, 22, 33, 44}`。

### 3. GPU kernel：debug 设备端

#### rocgdb

本机工具：`/opt/rocm/bin/rocgdb`

```bash
export LD_LIBRARY_PATH=/opt/rocm/lib:$LD_LIBRARY_PATH
rocgdb --args ./build/run_vec_add build/vec_add.co
```

```text
break vec_add
run
info registers
```

如需更好体验，可在编译时加 debug info：

```bash
make clean
make obj co host CLANG_GPU_FLAGS='--target=amdgcn-amd-amdhsa -mcpu=gfx942 -O2 -x c -g'
```

#### kernel 里 printf

在 `kernel.c` 加：

```c
if (i == 0)
    printf("vec_add: n=%d a=%p b=%p c=%p\n", n, a, b, c);
```

重新 `make co` 再运行，可确认 kernarg 是否传对。

### 4. 对照汇编理解 launch

`host.cpp` 的 launch 参数：

```text
grid  = (1, 1, 1)    → 1 个 work-group
block = (n, 1, 1)    → n 个 work-item（n=4 时 4 个 lane 工作）
```

对照 `build/vec_add.s` 的执行路径：

```text
s_load_dword s2, ...       ← 从 kernarg 读 n
v_cmp_gt_i32 vcc, s2, v0   ← v0 = workitem.id.x，边界检查
global_load_dword ...      ← 读 a[i], b[i]
v_add_f32 ...              ← 加
global_store_dword ...     ← 写 c[i]
s_endpgm                   ← wave 结束
```

若结果不对，按顺序查：

1. kernarg（`s_load` 读到的指针和 `n`）
2. 边界检查（是否越界）
3. 设备内存（`hipMemcpy` 是否成功）

### 5. 常见问题

| 现象 | 可能原因 | 怎么查 |
|---|---|---|
| `hipModuleLoadData` 失败 | `.co` 损坏或架构不匹配 | `file build/vec_add.co`，确认 target 是 `gfx942` |
| `hipModuleGetFunction` 失败 | 符号名不对 | `llvm-readobj --symbols build/vec_add.co` |
| `no ROCm-capable device` | 驱动 / GPU 未就绪 | `rocminfo` / `hipinfo` |
| 结果全 0 | launch 未完成就读回 | 确认有 `hipDeviceSynchronize()` |
| 结果不对 | `args[]` 传参错误 | GDB 看是否为 `&da,&db,&dc,&n` |
| hang | GPU 队列卡住 | `HIP_LAUNCH_BLOCKING=1` + `dmesg` |

### 6. 推荐 debug 顺序

```text
1. make disasm              确认 ISA 合理
2. make host                确认 host 能编译
3. ./build/run_vec_add build/vec_add.co
4. 不对 → GDB 断在 hipModuleLaunchKernel
5. 仍不对 → kernel printf 或 rocgdb 断在 vec_add
6. 对照 vec_add.s 逐步理解 s_load / global_load 行为
```

### 7. CLR / ROCr runtime 层 debug

本 demo 只覆盖 **HIP API → kernel 执行** 的最小路径。若问题出在 **stream/queue/AQL/signal 调度**（例如 queue idle hang、`hsa_signal_wait` 阻塞），需要 debug CLR（libamdhip64）和 ROCr（libhsa-runtime64）运行时：

→ 完整指南：[rocm-system-pyuan/DEBUG_CLR_ROCR.md](../../rocm-system-pyuan/DEBUG_CLR_ROCR.md)

要点：

- 用 `build_rocm.sh` 构建带符号的 `dist/libamdhip64.so.7` + `libhsa-runtime64.so.1.*`
- `AMD_LOG_LEVEL` / `AMD_LOG_MASK` 跟踪 AQL/queue/signal
- `rocgdb -p <pid>` 断在 `VirtualGPU::IsQueueIdle`、`InterceptQueue::Submit`
- hang 时用 `debug_scripts/06_collect_all_gpu_debug.sh --pid <pid>` 采集现场
