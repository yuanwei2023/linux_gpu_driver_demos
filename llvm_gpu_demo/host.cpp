/*
 * Host 端：加载手工编译出的 vec_add.co，用 hipModuleLaunchKernel 执行。
 * 演示“可执行 bin（code object）”如何被 runtime 加载。
 */
#include <hip/hip_runtime.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

#define HIPCHK(expr)                                                         \
	do {                                                                   \
		hipError_t _e = (expr);                                        \
		if (_e != hipSuccess) {                                        \
			std::cerr << __FILE__ << ":" << __LINE__ << " "        \
				  << hipGetErrorString(_e) << "\n";            \
			return 1;                                              \
		}                                                              \
	} while (0)

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

	std::vector<char> blob = read_file(argv[1]);

	hipModule_t mod{};
	hipFunction_t fn{};
	HIPCHK(hipModuleLoadData(&mod, blob.data()));
	HIPCHK(hipModuleGetFunction(&fn, mod, "vec_add"));

	int n = 4;
	float ha[] = {1, 2, 3, 4};
	float hb[] = {10, 20, 30, 40};
	float hc[4] = {};
	float *da = nullptr, *db = nullptr, *dc = nullptr;

	HIPCHK(hipMalloc(&da, n * sizeof(float)));
	HIPCHK(hipMalloc(&db, n * sizeof(float)));
	HIPCHK(hipMalloc(&dc, n * sizeof(float)));
	HIPCHK(hipMemcpy(da, ha, n * sizeof(float), hipMemcpyHostToDevice));
	HIPCHK(hipMemcpy(db, hb, n * sizeof(float), hipMemcpyHostToDevice));

	void *args[] = {&da, &db, &dc, &n};
	HIPCHK(hipModuleLaunchKernel(fn, 1, 1, 1, n, 1, 1, 0, 0, args, nullptr));
	HIPCHK(hipDeviceSynchronize());
	HIPCHK(hipMemcpy(hc, dc, n * sizeof(float), hipMemcpyDeviceToHost));
	HIPCHK(hipModuleUnload(mod));

	std::cout << "result:";
	for (int i = 0; i < n; i++)
		std::cout << " " << hc[i];
	std::cout << "\n";
	return 0;
}
