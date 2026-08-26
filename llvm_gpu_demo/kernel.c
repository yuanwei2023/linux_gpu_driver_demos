/*
 * kernel.c — 设备端（Device）GPU kernel 源码
 *
 * 编译目标不是 x86 host，而是 AMDGPU：
 *   clang --target=amdgcn-amd-amdhsa -mcpu=gfx942 ...
 *
 * 完整流水线（见 Makefile）：
 *   kernel.c → vec_add.ll (LLVM IR)
 *            → vec_add.s  (GCN 汇编，人类可读 ISA)
 *            → vec_add.o  (relocatable ELF，单 kernel 目标文件)
 *            → vec_add.co (code object，runtime 可加载的 GPU 二进制)
 *
 * host 端通过 hipModuleLoadData(vec_add.co) 加载本文件编译出的机器码，
 * 再 hipModuleLaunchKernel("vec_add", ...) 派发到 GPU 执行。
 *
 * 本 kernel 做的事：并行向量加  c[i] = a[i] + b[i]
 */

/*
 * __attribute__((amdgpu_kernel))
 *   告诉 clang 这是一个 GPU kernel 入口，而不是普通 host 函数。
 *   编译后会生成：
 *     - ELF 符号名 "vec_add"
 *     - .amdhsa_kernel 段里的 metadata（kernarg 布局、segment 大小等）
 *   runtime 正是通过符号名 "vec_add" 来查找函数句柄的。
 */
__attribute__((amdgpu_kernel))
void vec_add(const float *a, const float *b, float *c, int n)
{
	/*
	 * __builtin_amdgcn_workitem_id_x()
	 *   返回当前 workitem 在 X 维的 lane ID（0, 1, 2, ...）。
	 *   一个 wavefront 有 64 个 lane，每个 lane 跑同一份代码、不同 i。
	 *   对应 GCN 汇编里的 v0 = workitem.id.x（见 build/vec_add.s）。
	 *
	 * 注意：这是“设备端内置函数”，只在 amdgcn 目标下存在。
	 */
	int i = __builtin_amdgcn_workitem_id_x();

	/*
	 * 边界检查：launch 时 grid 可能大于 n（例如 n=4 但 block=64），
	 * 多余的 lane 必须提前 return，否则会越界访问 a/b/c。
	 * 汇编里对应 v_cmp + s_cbranch_execz。
	 */
	if (i < n)
		c[i] = a[i] + b[i];

	/*
	 * 指针 a/b/c 和 n 来自 kernarg（kernel argument）。
	 * launch 时 host 传入的是设备侧指针 da/db/dc 和 n 的地址；
	 * GPU 硬件通过 s_load_dword / s_load_dwordx4 从 kernarg 区读出这些值
	 * （见 vec_add.s 开头的 s_load 指令）。
	 *
	 * global_load/store 访问的是 GPU 可见的 global memory（VRAM/GTT），
	 * 对应 host 里 hipMalloc 分配的设备内存。
	 */
}
