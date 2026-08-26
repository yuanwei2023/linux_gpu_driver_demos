# Linux GPU Driver Demos

面向 GPU 驱动调试的内核教学 demo 集合。每个子目录是一个独立可加载的内核模块，通过 `dmesg` 观察行为。

| Demo | 学什么 |
|---|---|
| [dma_resv_demo](dma_resv_demo/) | `dma_resv`：每块 buffer 的 fence 登记表、usage 偏序 |
| [ttm_demo](ttm_demo/) | TTM + GEM：SYSTEM / VRAM placement、eviction、move |
| [llvm_gpu_demo](llvm_gpu_demo/) | LLVM 编译链：C → IR → GCN 汇编 → code object → GPU 执行 |

## 环境要求

- Linux 内核头文件：`/lib/modules/$(uname -r)/build`
- 可加载 `ttm.ko`（`modprobe ttm`）
- **不需要**卸载 amdgpu；`ttm_demo` 使用独立的假 VRAM 设备

## 快速开始

```bash
cd dma_resv_demo && make && sudo insmod dma_resv_demo.ko && dmesg | tail
cd ../ttm_demo      && make && sudo insmod ttm_demo.ko      && dmesg | tail
sudo rmmod ttm_demo dma_resv_demo
```

## 推荐阅读顺序

1. `dma_resv_demo` — 理解 BO 上的依赖/sync
2. `ttm_demo` — 理解 TTM 如何在 SYSTEM 与 VRAM 之间管理 BO
3. `llvm_gpu_demo` — 理解 kernel 如何从 LLVM IR 变成 GCN ISA 和 code object

## LLVM GPU demo（不涉及 insmod）

```bash
cd llvm_gpu_demo
./build.sh          # 只生成 IR / asm / .co，不跑 GPU
make show           # 查看 build/ 产物
# 有 GPU 时: make run
```
