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
