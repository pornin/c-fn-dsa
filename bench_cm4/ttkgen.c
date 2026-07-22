#include <stdint.h>
#include <string.h>

#include "timing.h"
#include "../fndsa.h"

static inline uint32_t
dec32le(const void *src)
{
	const uint8_t *buf = src;
	return (uint32_t)buf[0]
		| ((uint32_t)buf[1] << 8)
		| ((uint32_t)buf[2] << 16)
		| ((uint32_t)buf[3] << 24);
}

static inline void
enc32le(void *dst, uint32_t x)
{
	uint8_t *buf = dst;
	buf[0] = (uint8_t)x;
	buf[1] = (uint8_t)(x >> 8);
	buf[2] = (uint8_t)(x >> 16);
	buf[3] = (uint8_t)(x >> 24);
}

#if !FNDSA_ASM_CORTEXM4
/* We must include a dummy fndsa_fpr_add_sub() for benchmark purposes, in
   case assembly routines have been disabled; otherwise, there will be a
   link error.
   The dummy does an addition and a subtraction, so that the overall cost
   is close to what the real code sequence would do; the two output values
   are XORed so that result fits in the ABI, and the compiler does not
   elide any call. */
uint64_t
fndsa_fpr_add_sub(fpr x, fpr y)
{
	fpr a = fpr_add(x, y);
	fpr b = fpr_sub(x, y);
	return a ^ b;
}
#endif

int
main(void)
{
	system_init();

	/* Statically allocated arrays for objects (so that stack use
	   remains small). */
	static uint8_t skey[FNDSA_SIGN_KEY_SIZE(10)];
	static uint8_t vkey[FNDSA_VRFY_KEY_SIZE(10)];
	static uint8_t tmp[22559];

	/* For each degree, we make one measurement with a reproducible
	   seed value. */
	for (unsigned logn = 8; logn <= 10; logn ++) {
		uint8_t seed[2];
		seed[0] = (uint8_t)logn;
		switch (logn) {
		case 8:  seed[1] = 1; break;
		case 9:  seed[1] = 3; break;
		default: seed[1] = 0; break;
		}
		uint32_t begin, end;

		begin = get_system_ticks();
		if (!fndsa_keygen_seeded_temp(logn, seed, sizeof seed,
			skey, vkey, tmp, sizeof tmp))
		{
			prf("ERR keygen\n");
			system_exit();
		}
		end = get_system_ticks();
		uint32_t time_kgen = end - begin;

		prf("FN-DSA(n = %4u)  kgen: %9u\n", 1u << logn, time_kgen);

		/*
		prf("sign_key_size = %u\n", FNDSA_SIGN_KEY_SIZE(logn));
		prf("sign_key = ");
		for (size_t i = 0; i < FNDSA_SIGN_KEY_SIZE(logn); i ++) {
			prf("%02X", skey[i]);
		}
		prf("\n");
		prf("vrfy_key_size = %u\n", FNDSA_VRFY_KEY_SIZE(logn));
		prf("vrfy_key = ");
		for (size_t i = 0; i < FNDSA_VRFY_KEY_SIZE(logn); i ++) {
			prf("%02X", vkey[i]);
		}
		prf("\n");
		*/
	}

	/* Long-run measurements. */
	uint64_t total_kgen[3] = { 0, 0, 0 };
	for (uint32_t total_num = 1;; total_num ++) {
		for (unsigned logn = 8; logn <= 10; logn ++) {
			uint8_t seed[5];
			seed[0] = (uint8_t)logn;
			enc32le(seed + 1, total_num);
			uint32_t begin, end;

			begin = get_system_ticks();
			if (!fndsa_keygen_seeded_temp(logn, seed, sizeof seed,
				skey, vkey, tmp, sizeof tmp))
			{
				prf("ERR keygen\n");
				system_exit();
			}
			end = get_system_ticks();
			uint32_t time_kgen = end - begin;

			total_kgen[logn - 8] += time_kgen;
		}

		if (total_num <= 5 || (total_num % 100) == 0) {
			uint32_t nu = total_num;
			uint32_t na = nu >> 1;
			prf("\nnum = %u\n", nu);
			for (unsigned logn = 8; logn <= 10; logn ++) {
				uint32_t tk;
				tk = (total_kgen[logn - 8] + na) / nu;
				prf("FN-DSA(n = %4u)  kgen: %9u\n",
					1u << logn, tk);
			}
		} else {
			prf(".");
		}
	}

	system_exit();
	return 0;
}
