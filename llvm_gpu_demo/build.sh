#!/usr/bin/env bash
# 分步演示 LLVM → AMDGPU 编译链。默认只生成中间产物，不运行 GPU。
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

echo "=== Step 0: toolchain ==="
ROCM="${ROCM:-/opt/rocm}"
export ROCM
make -s help | head -12
echo

echo "=== Step 1: C kernel -> LLVM IR ==="
make ir
sed -n '1,25p' build/vec_add.ll
echo

echo "=== Step 2: opt (LLVM middle-end) ==="
make opt
echo "wrote build/vec_add.opt.ll"
echo

echo "=== Step 3: C kernel -> GCN assembly ==="
make asm
sed -n '1,35p' build/vec_add.s
echo

echo "=== Step 4: relocatable ELF (.o) ==="
make obj
file build/vec_add.o
echo

echo "=== Step 5: link -> code object (.co) ==="
make co
file build/vec_add.co
echo

echo "=== Step 6: disassemble (ISA + machine code) ==="
make disasm
sed -n '1,30p' build/vec_add.disasm.txt
echo

echo "=== Done ==="
echo "Artifacts in $ROOT/build/"
echo "To run on GPU (optional): make host run"
