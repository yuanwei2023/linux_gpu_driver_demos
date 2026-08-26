/*
 * 最小 AMDGPU kernel：c[i] = a[i] + b[i]
 *
 * 这是“设备端 C”，不是普通 host C。
 * clang 以 amdgcn-amd-amdhsa 为目标编译，生成 GPU code object。
 */
__attribute__((amdgpu_kernel))
void vec_add(const float *a, const float *b, float *c, int n)
{
	int i = __builtin_amdgcn_workitem_id_x();

	if (i < n)
		c[i] = a[i] + b[i];
}
