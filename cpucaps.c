/*
 * cpucaps.c - dump RISC-V CPU capabilities of the machine running this.
 *
 * Uses:
 *   - uname(2)              for kernel/arch identification
 *   - getauxval(AT_HWCAP)   for the base ISA letters (I,M,A,F,D,C,V,...)
 *   - riscv_hwprobe(2)      for vendor/arch/imp IDs and extension flags
 *                           (Linux >= 6.4, syscall 258 on riscv)
 *   - /proc/cpuinfo         raw dump as a human-readable fallback
 *   - SIGILL trap probe     executes a raw vsetvli opcode and catches the
 *                           illegal-instruction trap if V isn't implemented;
 *                           reads vlenb (CSR 0xC22) on success for VLEN.
 *                           Works even on kernels too old for hwprobe.
 *
 * Note on "matrix" extensions: RISC-V's Matrix extension is still an
 * unratified draft with no standardized opcode/CSR and no toolchain/kernel
 * support, so there's nothing generic to probe for it here.
 *
 * Build:  gcc -O2 -o cpucaps cpucaps.c
 * Run:    ./cpucaps
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <sys/utsname.h>

#ifndef __NR_riscv_hwprobe
#define __NR_riscv_hwprobe 258
#endif

struct riscv_hwprobe {
	int64_t key;
	uint64_t value;
};

/* Keys/bits that have been stable since the hwprobe ABI was introduced
 * in Linux 6.4 (arch/riscv/include/uapi/asm/hwprobe.h). Extensions added
 * to the kernel later are not decoded by name here, but their raw bits
 * are still printed. */
#define RISCV_HWPROBE_KEY_MVENDORID       0
#define RISCV_HWPROBE_KEY_MARCHID         1
#define RISCV_HWPROBE_KEY_MIMPID          2
#define RISCV_HWPROBE_KEY_BASE_BEHAVIOR   3
#define   RISCV_HWPROBE_BASE_BEHAVIOR_IMA (1 << 0)
#define RISCV_HWPROBE_KEY_IMA_EXT_0       4
#define   RISCV_HWPROBE_IMA_FD            (1 << 0)
#define   RISCV_HWPROBE_IMA_C             (1 << 1)
#define   RISCV_HWPROBE_IMA_V             (1 << 2)
#define   RISCV_HWPROBE_EXT_ZBA           (1 << 3)
#define   RISCV_HWPROBE_EXT_ZBB           (1 << 4)
#define   RISCV_HWPROBE_EXT_ZBS           (1 << 5)
#define RISCV_HWPROBE_KEY_CPUPERF_0       5
#define   RISCV_HWPROBE_MISALIGNED_MASK        (7 << 0)
#define   RISCV_HWPROBE_MISALIGNED_UNKNOWN     (0 << 0)
#define   RISCV_HWPROBE_MISALIGNED_EMULATED    (1 << 0)
#define   RISCV_HWPROBE_MISALIGNED_SLOW        (2 << 0)
#define   RISCV_HWPROBE_MISALIGNED_FAST        (3 << 0)
#define   RISCV_HWPROBE_MISALIGNED_UNSUPPORTED (4 << 0)

static long riscv_hwprobe(struct riscv_hwprobe *pairs, size_t pair_count,
                           size_t cpu_count, unsigned long *cpus,
                           unsigned int flags)
{
	return syscall(__NR_riscv_hwprobe, pairs, pair_count, cpu_count,
	               cpus, flags);
}

static void print_hwcap(void)
{
	unsigned long hwcap = getauxval(AT_HWCAP);
	char letters[27];
	int n = 0;

	for (int bit = 0; bit < 26; bit++)
		if (hwcap & (1UL << bit))
			letters[n++] = 'A' + bit;
	letters[n] = '\0';

	printf("AT_HWCAP base ISA letters: %s (raw=0x%lx)\n", letters, hwcap);
}

static void print_hwprobe(void)
{
	struct riscv_hwprobe pairs[6] = {
		{ RISCV_HWPROBE_KEY_MVENDORID, 0 },
		{ RISCV_HWPROBE_KEY_MARCHID, 0 },
		{ RISCV_HWPROBE_KEY_MIMPID, 0 },
		{ RISCV_HWPROBE_KEY_BASE_BEHAVIOR, 0 },
		{ RISCV_HWPROBE_KEY_IMA_EXT_0, 0 },
		{ RISCV_HWPROBE_KEY_CPUPERF_0, 0 },
	};

	if (riscv_hwprobe(pairs, 6, 0, NULL, 0) != 0) {
		perror("riscv_hwprobe syscall unavailable");
		return;
	}

	printf("mvendorid:  0x%lx\n", (unsigned long)pairs[0].value);
	printf("marchid:    0x%lx\n", (unsigned long)pairs[1].value);
	printf("mimpid:     0x%lx\n", (unsigned long)pairs[2].value);
	printf("base IMA:   %s\n",
	       (pairs[3].value & RISCV_HWPROBE_BASE_BEHAVIOR_IMA) ? "yes" : "no");

	uint64_t ext = pairs[4].value;
	printf("extensions (raw=0x%lx):", (unsigned long)ext);
	if (ext & RISCV_HWPROBE_IMA_FD) printf(" FD");
	if (ext & RISCV_HWPROBE_IMA_C)  printf(" C");
	if (ext & RISCV_HWPROBE_IMA_V)  printf(" V");
	if (ext & RISCV_HWPROBE_EXT_ZBA) printf(" Zba");
	if (ext & RISCV_HWPROBE_EXT_ZBB) printf(" Zbb");
	if (ext & RISCV_HWPROBE_EXT_ZBS) printf(" Zbs");
	if (ext & ~(uint64_t)(RISCV_HWPROBE_IMA_FD | RISCV_HWPROBE_IMA_C |
	                       RISCV_HWPROBE_IMA_V | RISCV_HWPROBE_EXT_ZBA |
	                       RISCV_HWPROBE_EXT_ZBB | RISCV_HWPROBE_EXT_ZBS))
		printf(" (+more bits set, not decoded here)");
	printf("\n");

	unsigned long misaligned = pairs[5].value & RISCV_HWPROBE_MISALIGNED_MASK;
	const char *perf = "unknown";
	switch (misaligned) {
	case RISCV_HWPROBE_MISALIGNED_EMULATED:    perf = "emulated"; break;
	case RISCV_HWPROBE_MISALIGNED_SLOW:        perf = "slow"; break;
	case RISCV_HWPROBE_MISALIGNED_FAST:        perf = "fast"; break;
	case RISCV_HWPROBE_MISALIGNED_UNSUPPORTED: perf = "unsupported"; break;
	}
	printf("misaligned access perf: %s\n", perf);
}

static sigjmp_buf rvv_jmpbuf;

static void rvv_sigill_handler(int sig)
{
	(void)sig;
	siglongjmp(rvv_jmpbuf, 1);
}

/* Direct hardware probe for the V (vector) extension, independent of what
 * the kernel's ISA string or hwprobe() claims. Opcode 0x57 is the OP-V
 * major opcode, reserved solely for vector instructions in the base ISA,
 * so any hart without V *must* raise an illegal-instruction exception on
 * it. We execute the raw encoding for "vsetvli x0, x0, e8, m1, tu, mu"
 * (all-zero vtype immediate) and catch SIGILL if it traps. */
static void print_rvv_probe(void)
{
#ifdef __riscv
	struct sigaction sa, old_sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = rvv_sigill_handler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGILL, &sa, &old_sa);

	if (sigsetjmp(rvv_jmpbuf, 1) == 0) {
		unsigned long vlenb;

		asm volatile (".word 0x00007057\n\t" ::: "memory"); /* vsetvli */
		asm volatile ("csrr %0, 0xC22" : "=r"(vlenb));       /* vlenb */

		printf("RVV (vector): supported, VLEN = %lu bits (vlenb=%lu bytes)\n",
		       vlenb * 8, vlenb);
	} else {
		printf("RVV (vector): not supported (SIGILL trap on vsetvli)\n");
	}

	sigaction(SIGILL, &old_sa, NULL);
#else
	printf("RVV (vector): probe skipped (not built for riscv)\n");
#endif
}

static void print_cpuinfo(void)
{
	FILE *f = fopen("/proc/cpuinfo", "r");
	char line[256];

	if (!f) {
		perror("fopen /proc/cpuinfo");
		return;
	}
	while (fgets(line, sizeof(line), f))
		fputs(line, stdout);
	fclose(f);
}

int main(void)
{
	struct utsname u;

	if (uname(&u) == 0)
		printf("uname: sysname=%s release=%s machine=%s\n",
		       u.sysname, u.release, u.machine);

	putchar('\n');
	printf("=== getauxval(AT_HWCAP) ===\n");
	print_hwcap();

	putchar('\n');
	printf("=== riscv_hwprobe(2) ===\n");
	print_hwprobe();

	putchar('\n');
	printf("=== RVV runtime probe ===\n");
	print_rvv_probe();
	printf("Matrix extension: not probed (unratified draft, no standard opcode/CSR)\n");

	putchar('\n');
	printf("=== /proc/cpuinfo ===\n");
	print_cpuinfo();

	return 0;
}
