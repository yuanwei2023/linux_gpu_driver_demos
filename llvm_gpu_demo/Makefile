# =============================================================================
# LLVM → AMDGPU 编译链 Demo — Makefile
#
# 把 kernel.c 一步步编译成可加载的 GPU code object，并可选运行。
#
# 流水线概览：
#   kernel.c
#     │ clang -emit-llvm          → build/vec_add.ll     (LLVM IR，SSA 中间表示)
#     │ opt -O2                   → build/vec_add.opt.ll (优化后的 IR)
#     │ clang -S                  → build/vec_add.s      (GCN 汇编，可读 ISA)
#     │ clang -c                  → build/vec_add.o      (relocatable ELF)
#     │ ld.lld -shared            → build/vec_add.co     (code object，runtime 加载)
#     └ llvm-objdump              → build/vec_add.disasm.txt (机器码反汇编)
#
#   host.cpp + vec_add.co → build/run_vec_add → GPU 执行
#
# 常用命令：
#   make ir asm obj co    # 只生成中间产物，不需要 GPU
#   make disasm           # 生成带十六进制机器码的反汇编
#   make run              # 编译 host 并在 GPU 上跑（需要 ROCm + GPU）
# =============================================================================

# ROCm 安装路径；本机通常是 /opt/rocm 或 /opt/rocm-7.x.x
ROCM        ?= /opt/rocm
LLVM_BIN    ?= $(ROCM)/llvm/bin
HIPCC       ?= $(ROCM)/bin/hipcc

# GPU 架构：MI300 系列为 gfx942；其他卡请改成对应 gfx 代号
GPU_TARGET  ?= gfx942
# AMDGPU 的 LLVM target triple；固定为 HSA code object 格式
GPU_TRIPLE  ?= amdgcn-amd-amdhsa

CLANG       := $(LLVM_BIN)/clang
OPT         := $(LLVM_BIN)/opt
LLD         := $(LLVM_BIN)/ld.lld
OBJDUMP     := $(LLVM_BIN)/llvm-objdump

BUILD       := build
KERNEL      := kernel.c
KERNEL_NAME := vec_add

# clang 编译 GPU kernel 的公共 flags：
#   --target=amdgcn-amd-amdhsa  生成 AMDGPU HSA code object 格式
#   -mcpu=gfx942               针对具体 GPU ISA 生成指令
#   -O2                         优化级别（影响 IR 和汇编）
#   -x c                        按 C 语言解析（即使不用 .c 后缀）
CLANG_GPU_FLAGS := --target=$(GPU_TRIPLE) -mcpu=$(GPU_TARGET) -O2 \
		   -x c -ffunction-sections -fdata-sections

.PHONY: all ir opt asm obj co disasm host run clean show help

all: co disasm

help:
	@echo "Targets:"
	@echo "  make ir      LLVM IR (.ll)         — 看 clang 前端输出"
	@echo "  make opt     opt 优化后的 IR        — 看 middle-end 优化"
	@echo "  make asm     GCN 汇编 (.s)          — 人类可读的 GPU ISA"
	@echo "  make obj     relocatable ELF (.o)   — 单 kernel 目标文件"
	@echo "  make co      code object (.co)      — runtime 可加载的 GPU bin"
	@echo "  make disasm  反汇编 + 机器码        — 对照 ISA 和 hex"
	@echo "  make host    HIP host 加载器        — 编译 run_vec_add"
	@echo "  make run     在 GPU 上执行          — 需要 ROCm + GPU"
	@echo "  make show    列出 build/ 产物"
	@echo ""
	@echo "Variables: GPU_TARGET=$(GPU_TARGET) ROCM=$(ROCM)"

$(BUILD):
	mkdir -p $(BUILD)

# ── Step 1: C → LLVM IR ─────────────────────────────────────────────────────
# -S -emit-llvm：只输出 IR，不进入后端 codegen
# 产物 vec_add.ll 里能看到 workitem.id、addrspace(1) 等 AMDGPU 特有 IR
ir: $(BUILD)/$(KERNEL_NAME).ll
opt: $(BUILD)/$(KERNEL_NAME).opt.ll

$(BUILD)/$(KERNEL_NAME).ll: $(KERNEL) | $(BUILD)
	$(CLANG) $(CLANG_GPU_FLAGS) -S -emit-llvm $< -o $@

# ── Step 2: IR → 优化 IR（可选，LLVM middle-end）────────────────────────────
# opt 跑 O2 pass pipeline；对比 .ll 和 .opt.ll 可看优化效果
$(BUILD)/$(KERNEL_NAME).opt.ll: $(BUILD)/$(KERNEL_NAME).ll
	$(OPT) -S -passes='default<O2>' $< -o $@

# ── Step 3: C → GCN 汇编 ────────────────────────────────────────────────────
# -S：输出汇编而非机器码；直接阅读 s_/v_/global_load 等指令
asm: $(BUILD)/$(KERNEL_NAME).s

$(BUILD)/$(KERNEL_NAME).s: $(KERNEL) | $(BUILD)
	$(CLANG) $(CLANG_GPU_FLAGS) -S $< -o $@

# ── Step 4: C → relocatable ELF (.o) ────────────────────────────────────────
# -c：生成目标文件，含 .text 机器码和 .amdhsa_kernel metadata
obj: $(BUILD)/$(KERNEL_NAME).o

$(BUILD)/$(KERNEL_NAME).o: $(KERNEL) | $(BUILD)
	$(CLANG) $(CLANG_GPU_FLAGS) -c $< -o $@

# ── Step 5: .o → code object (.co) ──────────────────────────────────────────
# ld.lld -shared：链接成 HSA 可加载的 shared code object
# 这是 hipModuleLoadData() 需要的最终 GPU 二进制格式
co: $(BUILD)/$(KERNEL_NAME).co

$(BUILD)/$(KERNEL_NAME).co: $(BUILD)/$(KERNEL_NAME).o
	$(LLD) -shared $< -o $@

# ── Step 6: 反汇编（助记符 + 十六进制机器码）──────────────────────────────────
disasm: $(BUILD)/$(KERNEL_NAME).disasm.txt

$(BUILD)/$(KERNEL_NAME).disasm.txt: $(BUILD)/$(KERNEL_NAME).o
	$(OBJDUMP) -d --no-show-raw-insn $< > $@
	$(OBJDUMP) -d $< >> $@

# ── Host 端：编译加载器（x86 + HIP runtime）──────────────────────────────────
host: $(BUILD)/run_$(KERNEL_NAME)

$(BUILD)/run_$(KERNEL_NAME): host.cpp | $(BUILD)
	$(HIPCC) --offload-arch=$(GPU_TARGET) $< -o $@

# ── 在 GPU 上运行：host 加载 .co 并 launch vec_add ──────────────────────────
run: host co
	LD_LIBRARY_PATH=$(ROCM)/lib:$$LD_LIBRARY_PATH \
	HIP_VISIBLE_DEVICES=$${HIP_VISIBLE_DEVICES:-0} \
	$(BUILD)/run_$(KERNEL_NAME) $(BUILD)/$(KERNEL_NAME).co

show:
	@ls -lh $(BUILD)/ 2>/dev/null || echo "(run make first)"

clean:
	rm -rf $(BUILD)
