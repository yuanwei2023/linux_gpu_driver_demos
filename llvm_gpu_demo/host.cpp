/*
 * host.cpp — Host 端加载器
 *
 * 职责：把 Makefile 手工编译出的 vec_add.co（code object）加载到 GPU 并执行。
 *
 * 为什么不直接用 hipcc 编译 kernel.c？
 *   本 demo 刻意把“编译 kernel”和“运行 kernel”拆开，方便逐步查看：
 *     IR (.ll) → 汇编 (.s) → 目标文件 (.o) → code object (.co)
 *   日常开发中 hipcc 会把 device code 打包进 host 可执行文件，中间产物不可见。
 *
 * Code object (.co) 是什么？
 *   - ELF64 AMDGPU 格式的 GPU 可执行镜像（类似 GPU 侧的 .so）
 *   - 内含机器码 + .amdhsa_kernel metadata + 符号 "vec_add"
 *   - hipModuleLoadData() 解析并上传到 GPU code cache
 *
 * 编译本文件：
 *   make host
 * 运行（需要 ROCm + 可见 GPU）：
 *   make run
 *   或: LD_LIBRARY_PATH=/opt/rocm/lib:$LD_LIBRARY_PATH ./build/run_vec_add build/vec_add.co
 */
#include <hip/hip_runtime.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

/* 检查 HIP API 返回值，失败则打印错误并退出 */
#define HIPCHK(expr)                                                         \
	do {                                                                   \
		hipError_t _e = (expr);                                        \
		if (_e != hipSuccess) {                                        \
			std::cerr << __FILE__ << ":" << __LINE__ << " "        \
				  << hipGetErrorString(_e) << "\n";            \
			return 1;                                              \
		}                                                              \
	} while (0)

/*
 * 将整个 .co 文件读入内存。
 * hipModuleLoadData 需要的是内存中的 ELF 字节流，不是文件路径。
 * runtime 会解析其中的 kernel 符号和 metadata。
 */
static std::vector<char> read_file(const char *path)
{
	std::ifstream in(path, std::ios::binary);

	if (!in)
		throw std::runtime_error(path);
	return {std::istreambuf_iterator<char>(in), {}};
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::cerr << "usage: " << argv[0] << " build/vec_add.co\n";
		return 1;
	}

	/* ── 1. 加载 code object ─────────────────────────────────────── */
	std::vector<char> blob = read_file(argv[1]);

	/*
	 * hipModule_t  ≈  dlopen() 返回的 module 句柄
	 * hipFunction_t ≈  dlsym(module, "vec_add") 返回的函数指针
	 *
	 * LoadData 会把 .co 里的 GPU 指令上传到 device code cache，
	 * 并登记 kernel 名 "vec_add" 与入口地址、kernarg 布局的映射。
	 */
	hipModule_t mod{};
	hipFunction_t fn{};
	HIPCHK(hipModuleLoadData(&mod, blob.data()));
	HIPCHK(hipModuleGetFunction(&fn, mod, "vec_add"));

	/* ── 2. 准备 host 侧测试数据 ───────────────────────────────────── */
	int n = 4; /* 必须是变量：args[] 需要 &n，不能是 const */
	float ha[] = {1, 2, 3, 4};
	float hb[] = {10, 20, 30, 40};
	float hc[4] = {};
	float *da = nullptr, *db = nullptr, *dc = nullptr;

	/* ── 3. 分配设备内存并拷贝输入 ─────────────────────────────────── */
	HIPCHK(hipMalloc(&da, n * sizeof(float)));
	HIPCHK(hipMalloc(&db, n * sizeof(float)));
	HIPCHK(hipMalloc(&dc, n * sizeof(float)));
	HIPCHK(hipMemcpy(da, ha, n * sizeof(float), hipMemcpyHostToDevice));
	HIPCHK(hipMemcpy(db, hb, n * sizeof(float), hipMemcpyHostToDevice));

	/*
	 * ── 4. Launch kernel ────────────────────────────────────────────
	 *
	 * hipModuleLaunchKernel 参数说明：
	 *   fn                          kernel 函数句柄（vec_add）
	 *   gridDim  (1, 1, 1)          启动 1 个 work-group
	 *   blockDim (n, 1, 1)          每个 work-group 有 n 个 work-item
	 *   sharedMemBytes 0            不使用 LDS/shared memory
	 *   stream nullptr              默认 stream
	 *   args                        kernarg 指针数组（见下）
	 *   extra nullptr               无额外参数
	 *
	 * args[] 传的是“指针的地址”：
	 *   kernel 签名: vec_add(float *a, float *b, float *c, int n)
	 *   runtime 会把 &da, &db, &dc, &n 写入 kernarg buffer，
	 *   GPU 上 vec_add 通过 s_load 指令读出设备指针和 n。
	 *
	 * 期望结果：hc = {11, 22, 33, 44}
	 */
	void *args[] = {&da, &db, &dc, &n};
	HIPCHK(hipModuleLaunchKernel(fn,
				     /*grid*/  1, 1, 1,
				     /*block*/ n, 1, 1,
				     /*sharedMemBytes=*/0,
				     /*stream=*/0,
				     args,
				     /*extra=*/nullptr));

	/* 等待 GPU 完成（否则下面 memcpy 可能读到旧数据） */
	HIPCHK(hipDeviceSynchronize());

	/* ── 5. 读回结果并清理 ─────────────────────────────────────────── */
	HIPCHK(hipMemcpy(hc, dc, n * sizeof(float), hipMemcpyDeviceToHost));
	HIPCHK(hipModuleUnload(mod));

	std::cout << "result:";
	for (int i = 0; i < n; i++)
		std::cout << " " << hc[i];
	std::cout << "\n";
	return 0;
}
