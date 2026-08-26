#!/usr/bin/env bash
# =============================================================================
# build.sh — 分步演示 LLVM → AMDGPU 完整编译链
#
# 默认只生成 build/ 下的中间产物（IR / 汇编 / .o / .co / 反汇编），
# 不运行 GPU。适合在没有 GPU 或 amdgpu 未就绪时学习编译流程。
#
# 用法：
#   ./build.sh
#   ROCM=/opt/rocm-7.14.0 GPU_TARGET=gfx942 ./build.sh
#
# 有 GPU 时可选：
#   make host run
# =============================================================================
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

echo "=== Step 0: toolchain ==="
ROCM="${ROCM:-/opt/rocm}"
export ROCM
make -s help | head -12
echo

echo "=== Step 1: C kernel -> LLVM IR ==="
echo "  命令: clang --target=amdgcn-amd-amdhsa -emit-llvm kernel.c"
echo "  说明: IR 是 SSA 中间表示，opt 和后端都从这里继续"
make ir
sed -n '1,25p' build/vec_add.ll
echo

echo "=== Step 2: opt (LLVM middle-end) ==="
echo "  命令: opt -passes=default<O2> vec_add.ll"
echo "  说明: 跑 LLVM 优化 pass；对比 .ll 和 .opt.ll 可看优化效果"
make opt
echo "wrote build/vec_add.opt.ll"
echo

echo "=== Step 3: C kernel -> GCN assembly ==="
echo "  命令: clang -S kernel.c"
echo "  说明: 直接输出 GCN 汇编；s_*=标量, v_*=向量, global_load/store=访存"
make asm
sed -n '1,35p' build/vec_add.s
echo

echo "=== Step 4: relocatable ELF (.o) ==="
echo "  命令: clang -c kernel.c"
echo "  说明: 含机器码 + .amdhsa_kernel metadata，尚不能直接 load"
make obj
file build/vec_add.o
echo

echo "=== Step 5: link -> code object (.co) ==="
echo "  命令: ld.lld -shared vec_add.o -o vec_add.co"
echo "  说明: .co 是 hipModuleLoadData() 需要的 GPU 可执行镜像"
make co
file build/vec_add.co
echo

echo "=== Step 6: disassemble (ISA + machine code) ==="
echo "  命令: llvm-objdump -d vec_add.o"
echo "  说明: 每条指令附带十六进制机器码，便于对照 ISA 手册"
make disasm
sed -n '1,30p' build/vec_add.disasm.txt
echo

echo "=== Done ==="
echo "Artifacts in $ROOT/build/"
echo ""
echo "下一步（需要 GPU）："
echo "  export LD_LIBRARY_PATH=$ROCM/lib:\$LD_LIBRARY_PATH"
echo "  make host run"
