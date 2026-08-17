/*
 * rvv_add.c - minimal RVV 1.0 intrinsics smoke test: c = a + b (float32).
 *
 * The CI runner's actual hardware has no vector unit (confirmed by the
 * SIGILL trap probe in cpucaps.c), so this is built with -march=...v and
 * run under `qemu-riscv64 -cpu rv64,v=true` user-mode emulation instead
 * of on bare metal.
 *
 * Build:  clang -O2 -march=rv64gcv -o rvv_add rvv_add.c
 * Run:    qemu-riscv64 -cpu rv64,v=true,vlen=256,vext_spec=v1.0 ./rvv_add
 */
#include <stdio.h>
#include <riscv_vector.h>

int main(void)
{
	const size_t n = 8;
	float a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	float b[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };
	float c[8];

	size_t vl = __riscv_vsetvl_e32m1(n);
	vfloat32m1_t va = __riscv_vle32_v_f32m1(a, vl);
	vfloat32m1_t vb = __riscv_vle32_v_f32m1(b, vl);
	vfloat32m1_t vc = __riscv_vfadd_vv_f32m1(va, vb, vl);
	__riscv_vse32_v_f32m1(c, vc, vl);

	printf("vl=%zu\n", vl);

	int ok = 1;
	for (size_t i = 0; i < n; i++) {
		printf("c[%zu] = %g\n", i, c[i]);
		if (c[i] != a[i] + b[i])
			ok = 0;
	}

	printf(ok ? "PASS\n" : "FAIL\n");
	return ok ? 0 : 1;
}
