/*
 * Top-level signature generation functions.
 */

#include "sign_inner.h"

/* Verified properties at this point:
      degree is acceptable
      encoded signing key has the proper size
      signature buffer is large enough to receive the result
      tmp is large enough (but not necessarily aligned)  */
static size_t
sign_step1(unsigned logn, const uint8_t *sign_key, const uint8_t *mu,
	const uint8_t *seed, size_t seed_len, uint8_t *sig, void *tmp)
{
	size_t n = (size_t)1 << logn;

	/* Align tmp to a 32-byte boundary. */
	tmp = (void *)(((uintptr_t)tmp + 31) & ~(uintptr_t)31);

	/* We decode f, g and F into a temporary area, and use them
	   to recompute G. Only G will be provided in decoded format
	   to sign_core(); f, g and F can be redecoded cheaply from
	   the encoded key when needed. */
	int8_t *f = (int8_t *)tmp + 4 * n;
	int8_t *g = f + n;
	int8_t *F = g + n;
	int8_t *G = (int8_t *)tmp + ((size_t)58 << logn);

	/* Decode the private key. Header byte and length have already
	   been verified. */
	unsigned nbits;
	switch (logn) {
	case 2: case 3: case 4: case 5:
		nbits = 8;
		break;
	case 6: case 7:
		nbits = 7;
		break;
	case 8: case 9:
		nbits = 6;
		break;
	default:
		nbits = 5;
		break;
	}
	size_t k, j = 1;
	k = trim_i8_decode(logn, sign_key + j, f, nbits);
	if (k == 0) {
		return 0;
	}
	j += k;
	k = trim_i8_decode(logn, sign_key + j, g, nbits);
	if (k == 0) {
		return 0;
	}
	j += k;
	k = trim_i8_decode(logn, sign_key + j, F, 8);
	if (k == 0) {
		return 0;
	}

	/* Rebuild G and the public polynomial h:
	      h = g/f mod X^n+1 mod q
	      G = h*F mod X^n+1 mod q
	   We also compute the SHAKE256 hash of the verifying key. */
	uint16_t *t1 = (uint16_t *)tmp;
	uint16_t *t0 = t1 + n;
	/* t0 <- h = g/f */
	mqpoly_small_to_int(logn, g, t0);
	mqpoly_small_to_int(logn, f, t1);
	mqpoly_int_to_ntt(logn, t0);
	mqpoly_int_to_ntt(logn, t1);
	if (!mqpoly_div_ntt(logn, t0, t1)) {
		/* f is not invertible; the key is not valid */
		return 0;
	}
	/* t1 <- G = h*F */
	mqpoly_small_to_int(logn, F, t1);
	mqpoly_int_to_ntt(logn, t1);
	mqpoly_mul_ntt(logn, t1, t0);
	mqpoly_ntt_to_int(logn, t1);
	if (!mqpoly_int_to_small(logn, t1, G)) {
		/* coefficients of G are out-of-range */
		return 0;
	}

	/* Note: at this point we have h (in NTT representation); we could
	   use it to encode, then recompute the verifying key hash. This
	   is not mandated by FIPS 206, though; the signing key is assumed
	   to be internally consistent. */

	uint8_t seedbuf[40];

	/* For hedging support, we need the hash of the private key. We
	   can use tmp for the SHAKE256 context (tmp has size at least 232
	   bytes, while a shake_context structure is 208 bytes, and tmp
	   is suitably aligned). */
	shake_context *sc = (shake_context *)tmp;
	shake_init(sc, 256);
	shake_inject(sc, sign_key + 1, nbits << (logn - 2));
	shake_flip(sc);
	shake_extract(sc, seedbuf, sizeof seedbuf);

	/* The derived seed is obtained as:
	     SHAKE256(SHAKE256(f||g)[40] || mu || seed)[40]
	   We already have SHAKE256(f||g)[40] in seedbuf[]. */
	shake_init(sc, 256);
	shake_inject(sc, seedbuf, sizeof seedbuf);
	shake_inject(sc, mu, 64);

	/* We need some entropy. If none was provided, then we use the
	   system RNG. */
	if (seed == NULL) {
		if (!sysrng(seedbuf, sizeof seedbuf)) {
			return 0;
		}
		seed = seedbuf;
		seed_len = sizeof seedbuf;
	}
	shake_inject(sc, seed, seed_len);
	shake_flip(sc);
	shake_extract(sc, seedbuf, sizeof seedbuf);

	/* We now have G, and we checked that f, g and F can be decoded
	   successfully (no out-of-range element). We have a 40-byte seed.
	   We can proceed to the main signing loop. */
	return sign_core(logn, sign_key + 1, G, mu,
		seedbuf, sizeof seedbuf, sig, tmp);

	/* TODO: maybe explicitly overwrite the whole temporary area with
	   zeros? Arguably this is mostly wasted time if the area is
	   allocated on the stack, and if tmp is provided explicitly then
	   it is the responsibility of the caller to do any appropriate
	   zeroizing. */
}

/* Custom wrappers to allocate the temporary buffers on the stack. Several
   wrappers are defined so that stack allocation is not always worst-case. */
#define SIGN_WRAP(sz)   \
	NOINLINE static size_t sign_ ## sz(unsigned logn, \
		const uint8_t *sign_key, const uint8_t *mu, \
		const uint8_t *seed, size_t seed_len, \
		uint8_t *sig) \
	{ \
		uint8_t tmp[(sz) * 59 + 31]; \
		return sign_step1(logn, \
			sign_key, mu, seed, seed_len, sig, tmp); \
	}

SIGN_WRAP(32)
SIGN_WRAP(64)
SIGN_WRAP(128)
SIGN_WRAP(256)
SIGN_WRAP(512)
SIGN_WRAP(1024)

static size_t
sign_wrapper(int weak,
	const uint8_t *sign_key, size_t sign_key_len,
	const uint8_t *ctx, size_t ctx_len,
	const char *id, const uint8_t *hv, size_t hv_len,
	const uint8_t *seed, size_t seed_len,
	uint8_t *sig, size_t max_sig_len,
	void *tmp, size_t tmp_len)
{
	/* Signing key defines the degree to use. */
	if (sign_key_len == 0) {
		return 0;
	}
	unsigned head = sign_key[0];
	if ((head & 0xF0) != 0x50) {
		return 0;
	}
	unsigned logn = head & 0x0F;
	if (weak) {
		if (logn < 2 || logn > 8) {
			return 0;
		}
	} else {
		if (logn < 9 || logn > 10) {
			return 0;
		}
	}
	if (sign_key_len != FNDSA_SIGN_KEY_SIZE(logn)) {
		return 0;
	}
	if (sig == NULL) {
		return FNDSA_SIGNATURE_SIZE(logn);
	}
	if (max_sig_len < FNDSA_SIGNATURE_SIZE(logn)) {
		return 0;
	}

	/* We have checked that the degree is acceptable, the signing key
	   size is correct, and the signature will fit in the output buffer.
	   We compute the message representative mu. */
	uint8_t mu[64];
	if (id != NULL && *(const uint8_t *)id == 0xFE) {
		/* External mu mode. */
		if (hv_len != 64) {
			return 0;
		}
		memcpy(mu, hv, hv_len);
	} else {
		if (!fndsa_compute_mu(mu,
			fndsa_hashed_vrfykey_from_signkey(
				sign_key, sign_key_len),
			ctx, ctx_len, id, hv, hv_len))
		{
			return 0;
		}
	}

	if (tmp == NULL) {
		switch (logn) {
		case 6:
			return sign_64(logn,
				sign_key, mu, seed, seed_len, sig);
		case 7:
			return sign_128(logn,
				sign_key, mu, seed, seed_len, sig);
		case 8:
			return sign_256(logn,
				sign_key, mu, seed, seed_len, sig);
		case 9:
			return sign_512(logn,
				sign_key, mu, seed, seed_len, sig);
		case 10:
			return sign_1024(logn,
				sign_key, mu, seed, seed_len, sig);
		default:
			return sign_32(logn,
				sign_key, mu, seed, seed_len, sig);
		}
	} else {
		if (tmp_len < (((size_t)59 << logn) + 31)) {
			return 0;
		}
		return sign_step1(logn,
			sign_key, mu, seed, seed_len, sig, tmp);
	}
}

/* see fndsa.h */
size_t
fndsa_sign(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	void *sig, size_t max_sig_len)
{
	return sign_wrapper(0, sign_key, sign_key_len,
		ctx, ctx_len, id, hv, hv_len,
		NULL, 0, sig, max_sig_len, NULL, 0);
}

/* see fndsa.h */
size_t
fndsa_sign_seeded(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	const void *seed, size_t seed_len,
	void *sig, size_t max_sig_len)
{
	return sign_wrapper(0, sign_key, sign_key_len,
		ctx, ctx_len, id, hv, hv_len,
		seed, seed_len, sig, max_sig_len, NULL, 0);
}

/* see fndsa.h */
size_t
fndsa_sign_temp(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	void *sig, size_t max_sig_len,
	void *tmp, size_t tmp_len)
{
	return sign_wrapper(0, sign_key, sign_key_len,
		ctx, ctx_len, id, hv, hv_len,
		NULL, 0, sig, max_sig_len, tmp, tmp_len);
}

/* see fndsa.h */
size_t
fndsa_sign_seeded_temp(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	const void *seed, size_t seed_len,
	void *sig, size_t max_sig_len,
	void *tmp, size_t tmp_len)
{
	return sign_wrapper(0, sign_key, sign_key_len,
		ctx, ctx_len, id, hv, hv_len,
		seed, seed_len, sig, max_sig_len, tmp, tmp_len);
}

/* see fndsa.h */
size_t
fndsa_sign_weak(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	void *sig, size_t max_sig_len)
{
	return sign_wrapper(1, sign_key, sign_key_len,
		ctx, ctx_len, id, hv, hv_len,
		NULL, 0, sig, max_sig_len, NULL, 0);
}

/* see fndsa.h */
size_t
fndsa_sign_weak_seeded(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	const void *seed, size_t seed_len,
	void *sig, size_t max_sig_len)
{
	return sign_wrapper(1, sign_key, sign_key_len,
		ctx, ctx_len, id, hv, hv_len,
		seed, seed_len, sig, max_sig_len, NULL, 0);
}

/* see fndsa.h */
size_t
fndsa_sign_weak_temp(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	void *sig, size_t max_sig_len,
	void *tmp, size_t tmp_len)
{
	return sign_wrapper(1, sign_key, sign_key_len,
		ctx, ctx_len, id, hv, hv_len,
		NULL, 0, sig, max_sig_len, tmp, tmp_len);
}

/* see fndsa.h */
size_t
fndsa_sign_weak_seeded_temp(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	const void *seed, size_t seed_len,
	void *sig, size_t max_sig_len,
	void *tmp, size_t tmp_len)
{
	return sign_wrapper(1, sign_key, sign_key_len,
		ctx, ctx_len, id, hv, hv_len,
		seed, seed_len, sig, max_sig_len, tmp, tmp_len);
}
