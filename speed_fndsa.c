/*
 * PERFORMANCE MEASUREMENTS
 * ========================
 *
 * WARNING 1: while the code tries to read "true" CPU clock cycles, it
 * might return meaningless figures in some situations. In general, you
 * should ensure the following:
 * 1. Use a relatively "idle" machine.
 * 2. Disable thermal-based frequency scaling, aka "TurboBoost" in Intel
 *    terminology.
 * 3. If possible, disable logical threads, aka "hyperthreading" (i.e.
 *    having more virtual cores than physical cores). Since the code
 *    below is monothreaded, this is not strictly necessary if the machine
 *    has at least two physical cores and is otherwise idle.
 * Some systems are asymmetrical structures, with some "performance" cores
 * that are faster but draw more power than the "economy" cores. No attempt
 * is made here to target a specific core and be locked to it. If your
 * system is asymmetrical, you may get varying results depending on what
 * core you ended up using (or, worse, if the kernel decided to migrate
 * your process between cores during the test). The code does include a
 * bit of warmup to try to avoid such things.
 * In any case, remember that benchmarks are not guarantees and only give
 * a crude approximation of how the measured code would fare when used in
 * any given context.
 *
 * WARNING 2: By default, this code uses performance counters to obtain
 * the actual CPU cycle count. In general, on a plain system, it will
 * crash with "illegal instruction" or "segmentation fault". Access to
 * the in-CPU cycle counter must first be allowed, which usually needs
 * an action from the superuser (root) but possibly a kernel module.
 * Operating systems prevent access (by default) to performance counters
 * because such counters may help local attacker exercise timing attacks
 * to extract information from processes that they should not be able to
 * access. On a personal, mono-user machine, allowing access to such
 * counters is much less an issue.
 *
 * USING THE TSC
 * =============
 * If this program is invoked with the "-tsc" command-line parameter, then
 * all measurements will use the "TSC", which is normally accessible from
 * userland without any special permission. The TSC has three drawbacks:
 *
 *  - It usually has a low resolution.
 *
 *  - On some systems, the nominal TSC frequency is not readily available,
 *    and a calibration loop is needed.
 *
 *  - It normally runs at a fixed frequency, even if the CPU actual frequency
 *    goes up and down in response to load and temperature; thus, benchmarks
 *    that use the TSC might report the code as running faster when the
 *    machine's temperature is lower.
 *
 * For these reasons, TSC-based benchmarking is not recommended, and is not
 * the default mode; you have to pass the "-tsc" parameter explicitly.
 *
 * ENABLING PERFORMANCE COUNTER ACCESS
 * ===================================
 * We list below some methods to allow this code to access the true cycle
 * counter.
 *
 * x86 (both 32-bit and 64-bit):
 * -----------------------------
 * The RDPMC instruction is used. Normally, such counters are inaccessible
 * from userland, but that access can be allowed on Linux by doing the
 * following (as root):
 *    echo 2 > /sys/bus/event_source/devices/cpu/rdpmc
 * Once done, the setting "sticks" until the next reboot.
 *
 * I do not know how to do the same on Windows; some Internet sources hint
 * at lack of any simple method, short of loading a custom kernel module.
 * This repository might be relevant (I have not tried it):
 *    https://github.com/intel/pcm
 *
 * Note that if you run in a virtual machine, then access to the performance
 * counters must probably be enabled on both guest and host.
 *
 * aarch64:
 * --------
 * On 64-bit ARM systems (aarch64), pmccntr_el0 is used. Enabling access
 * requires a custom Linux kernel module. I am using one I wrote, available
 * here:
 *    https://github.com/pornin/cycle-counter
 * On some systems (this probably depends on the Linux kernel version), the
 * module is not enough, because cores "forget" the setting when they enter
 * idle state. It can then be necessary to disable that state, with something
 * like that (as root, of course):
 *    echo 1 > /sys/devices/system/cpu/cpu0/cpuidle/state1/disable
 *    echo 1 > /sys/devices/system/cpu/cpu1/cpuidle/state1/disable
 *    echo 1 > /sys/devices/system/cpu/cpu2/cpuidle/state1/disable
 *    echo 1 > /sys/devices/system/cpu/cpu3/cpuidle/state1/disable
 * and only then load the module which enables userland access to the counter.
 * Disabling the idle state was necessary on an ODROID C4 (ARM Cortex-A55)
 * running Ubuntu 22.04 (kernel 4.9.337-13), but not on a Raspberry Pi 5
 * (ARM Cortex-A76) running Ubuntu 24.04 (kernel 6.8.0-1015-raspi).
 *
 * RISC-V:
 * -------
 * On RISC-V systems (riscv64), the rdcycle opcode is used. As in the
 * 64-bit ARM case, a custom kernel module must be used to enable access
 * from userland:
 *    https://github.com/pornin/cycle-counter
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fndsa.h"

/* Set to non-zero to fallback to the TSC, and report times in nanoseconds. */
static int use_tsc = 0;

#if defined __x86_64__ || defined _M_X64 || defined __i386__ || defined _M_IX86
/* ===== x86 ===== */

#include <immintrin.h>
#ifdef _MSC_VER
/* On Windows, the intrinsic is called __readpmc(), not __rdpmc(). But it
   will usually imply a crash, since Windows does not enable access to the
   performance counters. */
#ifndef __rdpmc
#define __rdpmc   __readpmc
#endif
#else
#include <x86intrin.h>
#include <cpuid.h>
#endif

#if defined __GNUC__ || defined __clang__
__attribute__((target("sse2")))
#endif
static inline uint64_t
core_cycles(void)
{
	_mm_lfence();
	if (use_tsc) {
		return __rdtsc();
	} else {
		return __rdpmc(0x40000001);
	}
}

static double
tsc_freq_system(void)
{
#ifdef _MSC_VER
	int rr[4];
	__cpuid(rr, 0);
	if (rr[0] >= 0x15) {
		__cpuid(rr, 0x15);
		unsigned ax = (unsigned)rr[0];
		unsigned bx = (unsigned)rr[1];
		unsigned cx = (unsigned)rr[2];
		if (bx != 0 && cx != 0) {
			return (double)bx * (double)cx / (double)ax;
		}
	}
#else
	unsigned ax, bx, cx, dx;
	if (__get_cpuid(0x15, &ax, &bx, &cx, &dx) && bx != 0 && cx != 0) {
		return (double)bx * (double)cx / (double)ax;
	}
#endif
	return 0.0;
}

#elif defined __aarch64__ && (defined __GNUC__ || defined __clang__)
/* ===== ARM (64-bit) ===== */

static inline uint64_t
core_cycles(void)
{
	uint64_t x;
	if (use_tsc) {
		__asm__ __volatile__ (
			"dsb sy\n\tmrs %0, cntvct_el0" : "=r" (x) : : );
	} else {
		__asm__ __volatile__ (
			"dsb sy\n\tmrs %0, pmccntr_el0" : "=r" (x) : : );
	}
	return x;
}

static double
tsc_freq_system(void)
{
	uint64_t x;
	__asm__ __volatile__ ("mrs %0, cntfrq_el0" : "=r" (x) : : );
	return (double)x;
}

#elif defined __riscv && defined __riscv_xlen && __riscv_xlen >= 64
/* ===== RISC-V (64-bit) ===== */

static inline uint64_t
core_cycles(void)
{
	uint64_t x;
	if (use_tsc) {
		__asm__ __volatile__ ("rdtime %0" : "=r" (x));
	} else {
		__asm__ __volatile__ ("rdcycle %0" : "=r" (x));
	}
	return x;
}

static double
tsc_freq_system(void)
{
	return 0.0;
}

#else
#error Architecture not supported (cycle counter access)
#endif

/* Get the multiplicative factor to convert TSC values to nanoseconds. */
static double
tsc_to_nanoseconds(void)
{
	double f = tsc_freq_system();
	if (f > 0.0) {
		printf("System-reported TSC frequency: %.2f\n", f);
		return 1000000000.0 / f;
	}

	/* We could not get the frequency information from CPUID.
	   We fallback to a wall-clock estimate. */
	printf("WARNING: TSC frequency not available; calibrating...\n");
	for (uint64_t ctr = 1000000;; ctr <<= 1) {
		clock_t t_begin = clock();
		uint64_t c_begin = core_cycles();
		for (uint64_t j = 0; j < ctr; j ++) {
			/* We do some work repeatedly (the CPU must not
			   go to idle state). */
			static volatile double zr = 1.0, zi = 0.0;
			double x = zr * 0.540302305868140
				- zi * 0.841470984807897;
			double y = zi * 0.540302305868140
				+ zr * 0.841470984807897;
			zr = x;
			zi = y;
		}
		uint64_t c_end = core_cycles();
		clock_t t_end = clock();
		double c = (double)(c_end - c_begin);
		double t = (double)(t_end - t_begin) / (double)CLOCKS_PER_SEC;
		if (t >= 1.0) {
			double tf = c / t;
			printf("Estimated TSC frequency: %.2f Hz\n", tf);
			return 1000000000.0 / tf;
		}
	}
}

static int
cmp_u64(const void *v1, const void *v2)
{
	uint64_t x1 = *(const uint64_t *)v1;
	uint64_t x2 = *(const uint64_t *)v2;
	if (x1 < x2) {
		return -1;
	} else if (x1 == x2) {
		return 0;
	} else {
		return 1;
	}
}

static double
bench_keygen(unsigned logn, unsigned *x)
{
	uint64_t z = core_cycles();
	uint8_t seed[8];
	for (int i = 0; i < 8; i ++) {
		seed[i] = (uint8_t)(z >> (i << 3));
	}
	uint8_t sk[FNDSA_SIGN_KEY_SIZE(10)];
	uint8_t vk[FNDSA_VRFY_KEY_SIZE(10)];
	uint64_t tt[100];
	for (int i = 0; i < 120; i ++) {
		uint64_t begin = core_cycles();
		fndsa_keygen_seeded(logn, seed, sizeof seed, sk, vk);
		seed[0] ^= sk[FNDSA_SIGN_KEY_SIZE(logn) - 1];
		seed[1] ^= vk[FNDSA_SIGN_KEY_SIZE(logn) - 1];
		uint64_t end = core_cycles();
		if (i >= 20) {
			tt[i - 20] = end - begin;
		}
	}
	qsort(tt, 100, sizeof(uint64_t), &cmp_u64);
	*x ^= seed[0] ^ seed[1];
	return (double)tt[50];
}

static double
bench_sign(unsigned logn, unsigned *x)
{
	uint64_t z = core_cycles();
	uint8_t seed[8];
	for (int i = 0; i < 8; i ++) {
		seed[i] = (uint8_t)(z >> (i << 3));
	}
	uint8_t sk[FNDSA_SIGN_KEY_SIZE(10)];
	uint8_t vk[FNDSA_VRFY_KEY_SIZE(10)];
	fndsa_keygen_seeded(logn, seed, sizeof seed, sk, vk);
	seed[0] ^= 0x01;
	uint64_t tt[100];
	uint8_t sig[FNDSA_SIGNATURE_SIZE(10)];
	for (int i = 0; i < 120; i ++) {
		uint64_t begin = core_cycles();
		fndsa_sign_seeded(sk, FNDSA_SIGN_KEY_SIZE(logn),
			NULL, 0, FNDSA_HASH_ID_RAW, "test", 4,
			seed, sizeof seed, sig, FNDSA_SIGNATURE_SIZE(logn));
		seed[1] ^= sig[1];
		uint64_t end = core_cycles();
		if (i >= 20) {
			tt[i - 20] = end - begin;
		}
	}
	qsort(tt, 100, sizeof(uint64_t), &cmp_u64);
	*x ^= seed[0] ^ seed[1];
	return (double)tt[50];
}

static double
bench_verify(unsigned logn, unsigned *x)
{
	uint64_t z = core_cycles();
	uint8_t seed[8];
	for (int i = 0; i < 8; i ++) {
		seed[i] = (uint8_t)(z >> (i << 3));
	}
	uint8_t sk[FNDSA_SIGN_KEY_SIZE(10)];
	uint8_t vk[FNDSA_VRFY_KEY_SIZE(10)];
	fndsa_keygen_seeded(logn, seed, sizeof seed, sk, vk);
	seed[0] ^= 0x01;
	uint64_t tt[100];
	uint8_t sig[120][FNDSA_SIGNATURE_SIZE(10)];
	for (int i = 0; i < 120; i ++) {
		fndsa_sign_seeded(sk, FNDSA_SIGN_KEY_SIZE(logn),
			NULL, 0, FNDSA_HASH_ID_RAW, "test", 4,
			seed, sizeof seed,
			sig[i], FNDSA_SIGNATURE_SIZE(logn));
		seed[2] ++;
	}
	uint8_t msg[5] = "test";
	for (int i = 0; i < 120; i ++) {
		uint64_t begin = core_cycles();
		int r = fndsa_verify(sig[i], FNDSA_SIGNATURE_SIZE(logn),
			vk, FNDSA_VRFY_KEY_SIZE(logn),
			NULL, 0, FNDSA_HASH_ID_RAW, msg, 4);
		msg[0] ^= r;
		uint64_t end = core_cycles();
		if (i >= 20) {
			tt[i - 20] = end - begin;
		}
	}
	qsort(tt, 100, sizeof(uint64_t), &cmp_u64);
	*x ^= seed[2] ^ msg[0];
	return (double)tt[50];
}

/* We also perform a few internal function benchmarks. */

#include "inner.h"
static double
bench_make_c(unsigned logn, unsigned *x)
{
	uint64_t z = core_cycles();
	uint8_t seed[8];
	for (int i = 0; i < 8; i ++) {
		seed[i] = (uint8_t)(z >> (i << 3));
	}
	uint16_t tmp[1024];
	memset(tmp, 0, sizeof tmp);
	memcpy(tmp, seed, sizeof seed);

	uint64_t tt[100];
	for (int i = 0; i < 120; i ++) {
		uint64_t begin = core_cycles();
		hash_to_point(logn, (uint8_t *)tmp, (uint8_t *)tmp + 40, tmp);
		uint64_t end = core_cycles();
		if (i >= 20) {
			tt[i - 20] = end - begin;
		}
	}
	qsort(tt, 100, sizeof(uint64_t), &cmp_u64);
	*x ^= tmp[0];
	return (double)tt[50];
}

static double
bench_NTT(unsigned logn, unsigned *x)
{
	uint64_t z = core_cycles();
	uint16_t z0 = z % 12289;
	uint16_t z1 = (z >> 16) % 12289;
	uint16_t z2 = (z >> 32) % 12289;
	uint16_t z3 = (z >> 48) % 12289;
	uint16_t tmp[1024];
	for (int i = 0; i < 1024; i += 4) {
		tmp[i + 0] = z0;
		tmp[i + 1] = z1;
		tmp[i + 2] = z2;
		tmp[i + 3] = z3;
	}
	mqpoly_ext_to_int(logn, tmp);

	uint64_t tt[100];
	for (int i = 0; i < 120; i ++) {
		uint64_t begin = core_cycles();
		mqpoly_int_to_ntt(logn, tmp);
		uint64_t end = core_cycles();
		if (i >= 20) {
			tt[i - 20] = end - begin;
		}
	}
	qsort(tt, 100, sizeof(uint64_t), &cmp_u64);
	*x ^= tmp[0];
	return (double)tt[50];
}

static double
bench_iNTT(unsigned logn, unsigned *x)
{
	uint64_t z = core_cycles();
	uint16_t z0 = z % 12289;
	uint16_t z1 = (z >> 16) % 12289;
	uint16_t z2 = (z >> 32) % 12289;
	uint16_t z3 = (z >> 48) % 12289;
	uint16_t tmp[1024];
	for (int i = 0; i < 1024; i += 4) {
		tmp[i + 0] = z0;
		tmp[i + 1] = z1;
		tmp[i + 2] = z2;
		tmp[i + 3] = z3;
	}
	mqpoly_ext_to_int(logn, tmp);

	uint64_t tt[100];
	for (int i = 0; i < 120; i ++) {
		uint64_t begin = core_cycles();
		mqpoly_ntt_to_int(logn, tmp);
		uint64_t end = core_cycles();
		if (i >= 20) {
			tt[i - 20] = end - begin;
		}
	}
	qsort(tt, 100, sizeof(uint64_t), &cmp_u64);
	*x ^= tmp[0];
	return (double)tt[50];
}

static void
usage(void)
{
	printf("usage: speed_fndsa [ -tsc ]\n");
	printf(
 "By default, hardware performance counters are accessed for measurements;\n");
	printf(
 "this is usually blocked by the operating system and triggers a crash.\n");
	printf(
 "Use the '-tsc' parameter to switch to the real-time clock (TSC)\n");
	exit(EXIT_FAILURE);
}

int
main(int argc, char *argv[])
{
	if (argc > 2) {
		usage();
	}
	if (argc == 2) {
		if (strcmp(argv[1], "-tsc") == 0) {
			use_tsc = 1;
		} else {
			usage();
		}
	}

	unsigned x;
	double fa;
	const char *unit;

	if (use_tsc) {
		fa = tsc_to_nanoseconds();
		unit = "ns";
		printf("WARNING: values in nanoseconds, not clock cycles\n");
	} else {
		fa = 1.0;
		unit = "cycles";
	}

	printf("hash-to-point (n = 512)        %13.2f %s\n",
		fa * bench_make_c(9, &x), unit);
	printf("hash-to-point (n = 1024)       %13.2f %s\n",
		fa * bench_make_c(10, &x), unit);
	printf("NTT (n = 512)                  %13.2f %s\n",
		fa * bench_NTT(9, &x), unit);
	printf("NTT (n = 1024)                 %13.2f %s\n",
		fa * bench_NTT(10, &x), unit);
	printf("iNTT (n = 512)                 %13.2f %s\n",
		fa * bench_iNTT(9, &x), unit);
	printf("iNTT (n = 1024)                %13.2f %s\n",
		fa * bench_iNTT(10, &x), unit);

	printf("FN-DSA keygen (n = 512)        %13.2f %s\n",
		fa * bench_keygen(9, &x), unit);
	printf("FN-DSA keygen (n = 1024)       %13.2f %s\n",
		fa * bench_keygen(10, &x), unit);
	printf("FN-DSA sign (n = 512)          %13.2f %s\n",
		fa * bench_sign(9, &x), unit);
	printf("FN-DSA sign (n = 1024)         %13.2f %s\n",
		fa * bench_sign(10, &x), unit);
	printf("FN-DSA verify (n = 512)        %13.2f %s\n",
		fa * bench_verify(9, &x), unit);
	printf("FN-DSA verify (n = 1024)       %13.2f %s\n",
		fa * bench_verify(10, &x), unit);

	/* Value x must be "used" to prevent the compiler from optimizing
	   away the calls. */
	printf("%u\n", x);
	return 0;
}
