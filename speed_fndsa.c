/*
 * Performance measurements
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
 * bit of warmup to avoid such things.
 * In any case, remember that benchmarks are not guarantees and only give
 * a crude approximation of how the measured code would fare when used in
 * any given context.
 *
 * WARNING 2: This code uses performance counters. In general, on a plain
 * system, it will crash with "illegal instruction" or "segmentation fault".
 * Access to the in-CPU cycle counter must first be allowed, which usually
 * needs an action from the superuser (root) but possibly a kernel module.
 * Operating systems prevent access (by default) to performance counters
 * because such counters may help local attacker exercise timing attacks to
 * extract information from processes that they should not be able to access.
 * On a personal, mono-user machine, allowing access to such counters is
 * much less an issue.
 *
 * x86 (both 32-bit and 64-bit):
 * =============================
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
 * ========
 * On 64-bit ARM systems (aarch64), pmccntr_el0 is used. Enabling access
 * requires a custom Linux kernel module. I am using a simplified version
 * of the module shown here:
 *    https://github.com/jerinjacobk/armv8_pmu_cycle_counter_el0
 * On some systems (this probably depends on the Linux kernel version), the
 * module is not enough, because core "forget" the setting when they enter
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
 * =======
 * On RISC-V systems (riscv64), the rdcycle opcode is used. The cycle
 * counter usually reads as zero, but if I run the process through the
 * 'perf' tool, then rdcycle returns meaningful values:
 *    perf stat -o /dev/null ./speed_fndsa
 * The perf tool must be kept in sync with the exact kernel version, and
 * if you use a custom kernel then you might have to recompile it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fndsa.h"

#if defined __x86_64__ || defined _M_X64 || defined __i386__ || defined _M_IX86
#include <immintrin.h>
#ifdef _MSC_VER
/* On Windows, the intrinsic is called __readpmc(), not __rdpmc(). But it
   will usually imply a crash, since Windows does no enable access to the
   performance counters. */
#ifndef __rdpmc
#define __rdpmc   __readpmc
#endif
#else
#include <x86intrin.h>
#endif

#if defined __GNUC__ || defined __clang__
__attribute__((target("sse2")))
#endif
static inline uint64_t
core_cycles(void)
{
	_mm_lfence();
#if defined FNDSA_TSC && FNDSA_TSC
	return __rdtsc();
#else
	return __rdpmc(0x40000001);
#endif
}
#elif defined __aarch64__ && (defined __GNUC__ || defined __clang__)
static inline uint64_t
core_cycles(void)
{
	uint64_t x;
	__asm__ __volatile__ ("dsb sy\n\tmrs %0, pmccntr_el0" : "=r" (x) : : );
	return x;
}
#elif defined __riscv && defined __riscv_xlen && __riscv_xlen >= 64
static inline uint64_t
core_cycles(void)
{
	uint64_t x;
	__asm__ __volatile__ ("rdcycle %0" : "=r" (x));
	return x;
}
#else
#error Architecture not supported (cycle counter)
#endif

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

int
main(void)
{
	unsigned x;

	printf("FN-DSA keygen (n = 512)        %13.2f\n", bench_keygen(9, &x));
	printf("FN-DSA keygen (n = 1024)       %13.2f\n", bench_keygen(10, &x));
	printf("FN-DSA sign (n = 512)          %13.2f\n", bench_sign(9, &x));
	printf("FN-DSA sign (n = 1024)         %13.2f\n", bench_sign(10, &x));
	printf("FN-DSA verify (n = 512)        %13.2f\n", bench_verify(9, &x));
	printf("FN-DSA verify (n = 1024)       %13.2f\n", bench_verify(10, &x));

	printf("%u\n", x);
	return 0;
}
