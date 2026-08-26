# LLVM → AMDGPU 编译链 demo
#
# 目标：逐步生成 IR / 汇编 / reloc object / code object (.co)
# 可选：编译 host 并加载 .co 运行（需要 GPU + ROCm runtime）

ROCM        ?= /opt/rocm
LLVM_BIN    ?= $(ROCM)/llvm/bin
HIPCC       ?= $(ROCM)/bin/hipcc
GPU_TARGET  ?= gfx942
GPU_TRIPLE  ?= amdgcn-amd-amdhsa

CLANG       := $(LLVM_BIN)/clang
OPT         := $(LLVM_BIN)/opt
LLD         := $(LLVM_BIN)/ld.lld
OBJDUMP     := $(LLVM_BIN)/llvm-objdump

BUILD       := build
KERNEL      := kernel.c
KERNEL_NAME := vec_add

CLANG_GPU_FLAGS := --target=$(GPU_TRIPLE) -mcpu=$(GPU_TARGET) -O2 \
		   -x c -ffunction-sections -fdata-sections

.PHONY: all ir opt asm obj co disasm host run clean show help

all: co disasm

help:
	@echo "Targets:"
	@echo "  make ir      LLVM IR (.ll)"
	@echo "  make opt     opt 处理后的 IR"
	@echo "  make asm     GCN 汇编 (.s)"
	@echo "  make obj     relocatable ELF (.o)"
	@echo "  make co      code object / hsaco (.co)"
	@echo "  make disasm  反汇编 + 机器码"
	@echo "  make host    HIP host 加载器"
	@echo "  make run     在 GPU 上执行（需要 ROCm + GPU）"
	@echo "  make show    打印各阶段产物路径"
	@echo ""
	@echo "Variables: GPU_TARGET=$(GPU_TARGET) ROCM=$(ROCM)"

$(BUILD):
	mkdir -p $(BUILD)

ir: $(BUILD)/$(KERNEL_NAME).ll
opt: $(BUILD)/$(KERNEL_NAME).opt.ll
asm: $(BUILD)/$(KERNEL_NAME).s
obj: $(BUILD)/$(KERNEL_NAME).o
co: $(BUILD)/$(KERNEL_NAME).co
disasm: $(BUILD)/$(KERNEL_NAME).disasm.txt
host: $(BUILD)/run_$(KERNEL_NAME)

$(BUILD)/$(KERNEL_NAME).ll: $(KERNEL) | $(BUILD)
	$(CLANG) $(CLANG_GPU_FLAGS) -S -emit-llvm $< -o $@

$(BUILD)/$(KERNEL_NAME).opt.ll: $(BUILD)/$(KERNEL_NAME).ll
	$(OPT) -S -passes='default<O2>' $< -o $@

$(BUILD)/$(KERNEL_NAME).s: $(KERNEL) | $(BUILD)
	$(CLANG) $(CLANG_GPU_FLAGS) -S $< -o $@

$(BUILD)/$(KERNEL_NAME).o: $(KERNEL) | $(BUILD)
	$(CLANG) $(CLANG_GPU_FLAGS) -c $< -o $@

$(BUILD)/$(KERNEL_NAME).co: $(BUILD)/$(KERNEL_NAME).o
	$(LLD) -shared $< -o $@

$(BUILD)/$(KERNEL_NAME).disasm.txt: $(BUILD)/$(KERNEL_NAME).o
	$(OBJDUMP) -d --no-show-raw-insn $< > $@
	$(OBJDUMP) -d $< >> $@

$(BUILD)/run_$(KERNEL_NAME): host.cpp | $(BUILD)
	$(HIPCC) --offload-arch=$(GPU_TARGET) $< -o $@

run: host co
	LD_LIBRARY_PATH=$(ROCM)/lib:$$LD_LIBRARY_PATH \
	HIP_VISIBLE_DEVICES=$${HIP_VISIBLE_DEVICES:-0} \
	$(BUILD)/run_$(KERNEL_NAME) $(BUILD)/$(KERNEL_NAME).co

show:
	@ls -lh $(BUILD)/ 2>/dev/null || echo "(run make first)"

clean:
	rm -rf $(BUILD)
