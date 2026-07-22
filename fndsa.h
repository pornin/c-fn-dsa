#ifndef FNDSA_H__
#define FNDSA_H__

#include <stddef.h>
#include <stdint.h>

/*
 * FN-DSA is parameterized by a degree ('n'), which is a power of two.
 * Standard degrees are 512 (security level I) and 1024 (security level
 * V). All APIs here specify the degree logarithmically as the 'logn'
 * parameter, an integer with a value from 2 to 10 (inclusive). Standard
 * degrees correspond to logn = 9 or 10; the two macros below make that
 * convention explicit. Lower degrees (logn = 2 to 8) are for tests and
 * research only, since they do not provide sufficient security.
 *
 * Key and signature sizes (in bytes) are shown below:
 *
 *  logn    n     sign-key  vrfy-key  signature  security
 * ------------------------------------------------------------------
 *    9    512      1345       897       666     level I (~128 bits)
 *   10   1024      2369      1793      1280     level V (~256 bits)
 *
 *    2      4        77         8        47     none
 *    3      8        89        15        52     none
 *    4     16       113        29        63     none
 *    5     32       161        57        82     none
 *    6     64       241       113       122     none
 *    7    128       417       225       200     very weak
 *    8    256       705       449       356     presumed weak
 *
 * The private key, used to generate signatures, is hereafter called the
 * "signing key". The public key, used to verify signatures, is the
 * "verifying key".
 */

#define FNDSA_LOGN_512     9
#define FNDSA_LOGN_1024   10

/*
 * Signing (private) key size, in bytes, for 2 <= logn <= 10.
 */
#define FNDSA_SIGN_KEY_SIZE(logn)   \
	(65u + ((12u - ((logn) >= 6) - ((logn) >= 8) - ((logn) >= 10)) \
	<< ((logn) - 2)))

/*
 * Verifying (public) key size, in bytes, for 2 <= logn <= 10.
 */
#define FNDSA_VRFY_KEY_SIZE(logn)   (1u + (7u << ((logn) - 2)))

/*
 * Signature size, in bytes, for 2 <= logn <= 10.
 */
#define FNDSA_SIGNATURE_SIZE(logn) \
	(44u + 3 * (256u >> (10 - (logn))) + 2 * (128u >> (10 - (logn))) \
	+ 3 * (64u >> (10 - (logn))) + 2 * (16u >> (10 - (logn))) \
	- 2 * (2u >> (10 - (logn))) - 8 * (1u >> (10 - (logn))))

/*
 * Each message to sign, or to verify, is provided as a triplet of three
 * values:
 *  - A domain separation context, which is an arbitrary binary string of
 *    length at most 255 bytes. The context pointer can be NULL if the
 *    length is zero.
 *  - An identifier for the hash function used to pre-hash the message.
 *  - The pre-hashed message.
 * The caller is responsible for pre-hashing the message; the signature
 * generation and verification work on the hash value only. A special
 * identifier is defined for a "raw" message, which is not pre-hashed:
 * the message itself is provided as "hash value".
 *
 * The hash identifier is normally a DER-encoded ASN.1 OID for the hash
 * function; it is provided in this API under a character string type,
 * though its length is actually inferred from its ASN.1 header. The
 * special identifier for a raw message is an empty string.
 */

/* Hash function identifier: none (raw message) */
#define FNDSA_HASH_ID_RAW        ""

/* Hash function identifier: SHA-256 */
#define FNDSA_HASH_ID_SHA256     "\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x01"

/* Hash function identifier: SHA-384 */
#define FNDSA_HASH_ID_SHA384     "\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x02"

/* Hash function identifier: SHA-512 */
#define FNDSA_HASH_ID_SHA512     "\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x03"

/* Hash function identifier: SHA-512-256 */
#define FNDSA_HASH_ID_SHA512_256 "\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x06"

/* Hash function identifier: SHA3-256 */
#define FNDSA_HASH_ID_SHA3_256   "\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x08"

/* Hash function identifier: SHA3-384 */
#define FNDSA_HASH_ID_SHA3_384   "\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x09"

/* Hash function identifier: SHA3-512 */
#define FNDSA_HASH_ID_SHA3_512   "\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x0A"

/* Hash function identifier: SHAKE128 */
#define FNDSA_HASH_ID_SHAKE128   "\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x0B"

/* Hash function identifier: SHAKE256 */
#define FNDSA_HASH_ID_SHAKE256   "\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x0C"

/* Hash function identifier: external mu
   The provided message value is really the 64-byte "mu" value. The function
   fndsa_compute_mu() can help with computing that value. */
#define FNDSA_HASH_ID_EXTMU      "\xFE"

/*
 * Generate new key pair. The operating system's RNG is used as source of
 * entropy. This function is supported on Windows and on various Unix-like
 * systems (including Linux and macOS). An error (0) is reported if the RNG
 * fails; otherwise, the function succeeds and returns 1. On error, the
 * two output buffers are filled with zeros, which are not valid encoding
 * of keys.
 *
 * The new signing (private) and verifying (public) keys are written,
 * encoded, into the two provided buffers. The keys' respective sizes
 * can be obtained from the FNDSA_SIGN_KEY_SIZE and FNDSA_VRFY_KEY_SIZE
 * macros.
 *
 * Logarithmic degree MUST be between 2 and 10. Only values 9 and 10 are
 * standard.
 */
int fndsa_keygen(unsigned logn, void *sign_key, void *vrfy_key);

/*
 * This is similar to fndsa_keygen(), except that the provided 'tmp'
 * area is used for temporary buffers, with size tmp_len bytes, instead
 * of using the stack. This function is provided for the benefit of
 * small embedded system with limited RAM space. The size of the required
 * temporary area depends on the requested degree:
 *
 *    logn   min tmp_len   security
 *   -----------------------------------------
 *      9      11295       standard (level I)
 *     10      22559       standard (level V)
 *
 *      2        119       none
 *      3        207       none
 *      4        383       none
 *      5        735       none
 *      6       1439       none
 *      7       2847       very weak
 *      8       5663       presumed weak
 *
 * (Formula is: 22*n+31 bytes, for degree n = 2^logn)
 * If the temporary area is too short then an error is returned (0).
 *
 * The tmp area MUST NOT overlap with the destination buffers sign_key
 * and vrfy_key.
 */
int fndsa_keygen_temp(unsigned logn, void *sign_key, void *vrfy_key,
	void *tmp, size_t tmp_len);

/*
 * Generate a new key pair from an initial seed. The process is
 * deterministic for a given seed value. The seed MUST have been generated
 * with enough entropy (i.e. 256 bits or so). By design, this function
 * never fails.
 *
 * The new signing (private) and verifying (public) keys are written,
 * encoded, into the two provided buffers. The keys' respective sizes
 * can be obtained from the FNDSA_SIGN_KEY_SIZE and FNDSA_VRFY_KEY_SIZE
 * macros.
 *
 * Logarithmic degree MUST be between 2 and 10. Only values 9 and 10 are
 * standard.
 *
 * Though the process of deriving the key pair from the seed is
 * deterministic, it is not fully specified, and may differ between
 * implementations. It may also change without notice between versions
 * of the code. Thus, applications MUST NOT store seeds instead of
 * encoded private keys.
 */
void fndsa_keygen_seeded(unsigned logn, const void *seed, size_t seed_len,
	void *sign_key, void *vrfy_key);

/*
 * This is similar to fndsa_keygen_seeded(), except that an explicit
 * temporary area is provided, to avoid bulky stack allocation. See
 * fndsa_keygen_temp() for guidance on the temporary area size. If the
 * temporary area is too small then the function fails and returns 0;
 * otherwise, it generates a key pair and returns 1.
 */
int fndsa_keygen_seeded_temp(unsigned logn,
	const void *seed, size_t seed_len, void *sign_key, void *vrfy_key,
	void *tmp, size_t tmp_len);

/*
 * Get the hashed verifying key from the signing key. This function
 * returns a pointer within the encoded signing key. If the source
 * signing key does not have a valid length, then NULL is returned. The
 * hashed verifying key has length 64 bytes.
 */
static const uint8_t *
fndsa_hashed_vrfykey_from_signkey(const void *sign_key, size_t sign_key_len)
{
	if (sign_key_len < 65) {
		return NULL;
	}
	return (const uint8_t *)sign_key + sign_key_len - 64;
}

/*
 * Get the hashed verifying key from the verifying key. This function
 * stores the hash value (64 bytes) into the provided hashed_vkey[]
 * buffer.
 */
void fndsa_hashed_vrfykey_from_vrfykey(void *hashed_vkey,
	const void *vrfy_key, size_t vrfy_key_len);

/*
 * Compute the message representative mu (64 bytes), given the hashed
 * verifying key (hashed_vkey, 64 bytes), context string (ctx, size ctx_len
 * up to 255 bytes) and raw or pre-hashed message (hv, size hv_len).
 *
 * Rules:
 *  - hashed_vkey has size 64 bytes. See fndsa_hashed_vrfykey_from_signkey()
 *    and fndsa_hashed_vrfykey_from_vrfykey() to get that value.
 *  - ctx can be NULL if ctx_len is zero. ctx_len MUST be between 0 and 255,
 *    inclusive.
 *  - If id is FNDSA_HASH_ID_RAW, then the raw message itself is provided
 *    in hv (size hv_len). Otherwise, hv should point to a hash value, and
 *    id should be the ASN.1 DER-encoded OBJECT IDENTIFIER that identifies
 *    the hash function; the various FNDSA_HASH_ID_* constants contain the
 *    identifiers for some standard hash functions.
 *  - The mu[] array may overlap the other arrays (hashed_vkey, ctx, hv).
 *
 * Returned value is 1 on success, 0 on error. An error is reported if the
 * context string is too large, or if the hash identifier is syntactically
 * incorrect.
 */
int fndsa_compute_mu(void *mu, const void *hashed_vkey,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len);

/*
 * Helper for a streamed variant of fndsa_compute_mu(); it may be useful
 * when processing large messages (obtained by chunks) but using FN-DSA
 * without pre-hashing. This function injects the hashed verifying key
 * and context string into the provided shake_cb() callback (which
 * supposedly implements SHAKE256, as per the FN-DSA specification).
 * shake_cb() receives as parameters, in that order: a pointer to some
 * state value (shake_cb_state), a pointer to some more input bytes, and
 * the number of input bytes. The caller would afterwards continue the
 * SHAKE256 computation by injecting the message itself; the SHAKE256
 * output (64 bytes) will be the message representative mu.
 *
 * Returned value is 1 on success, 0 on error. An error is reported if the
 * context string is too large.
 */
int fndsa_compute_mu_start(const void *hashed_vkey,
	const void *ctx, size_t ctx_len,
	void (*shake_cb)(void *state, const uint8_t *data, size_t data_len),
	void *shake_cb_state);

/*
 * Sign a message.
 *    sign_key, sign_key_len   signing key (encoded)
 *    ctx, ctx_len             domain separation context (at most 255 bytes)
 *    id                       identifier for the pre-hash function
 *    hv, hv_len               pre-hashed message (or raw message)
 *    sig, sig_max_len         output buffer for the signature
 * This function returns 0 on error; error conditions include: signing key
 * cannot be decoded; signing key is for a weak degree (i.e. not 512 nor
 * 1024); system random generator failed to provide the requested random
 * bytes; output buffer is not large enough for the signature.
 *
 * If id is FNDSA_HASH_ID_EXTMU, then ctx and ctx_len are ignored (and
 * can be zero), hv_len shall be exactly 64, and hv shall point to the
 * "message representative" mu of size 64 bytes (see fndsa_compute_mu()
 * for details on how to obtain that value). Otherwise, ctx, ctx_len,
 * id, hv and hv_len shall follow the rules documented in
 * fndsa_compute_mu().
 *
 * If sig is NULL, then the signature is not generated, but the signing key
 * is still decoded; returned value is then the signature length (in bytes),
 * or 0 on error (signing key cannot be decoded or uses a weak degree). If
 * sig is not NULL, then sig_max_len should be the maximum buffer size for
 * the signature; an error (0) is returned if that length is lower than the
 * signature size implied by the degree of the signing key. If the output
 * buffer is large enough, and the signature generation succeeds (i.e. the
 * system random generator does not fail), then the signature is written
 * in sig[] and the signature length (in bytes) is returned.
 */
size_t fndsa_sign(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	void *sig, size_t max_sig_len);

/*
 * This is similar to fndsa_sign(), except that instead of using the
 * system random generator, all random number generation is performed
 * from an internal deterministic PRNG seeded with the provided seed
 * value (of length seed_len bytes). It is the responsibility of the
 * caller to provide a seed with sufficient entropy (preferably 320 bits
 * or more, i.e. a seed of at least 40 bytes). The FN-DSA standard uses
 * 40-byte seeds.
 *
 * This function is provided mostly for testing purposes,
 * and for bare-metal systems with no OS-provided random generator.
 */
size_t fndsa_sign_seeded(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	const void *seed, size_t seed_len,
	void *sig, size_t max_sig_len);

/*
 * This is similar to fndsa_sign(), except that temporary storage will
 * use the provided tmp[] area (of size tmp_len bytes) instead of stack
 * buffers. This function is provided for the benefit of small embedded
 * systems that have only small stacks. Temporary area sizes are as
 * follows:
 *
 *    logn   min tmp_len   security
 *   -----------------------------------------
 *      9      30239       standard (level I)
 *     10      60447       standard (level V)
 *
 *      2        267       none
 *      3        503       none
 *      4        975       none
 *      5       1919       none
 *      6       3807       none
 *      7       7583       very weak
 *      8      15135       presumed weak
 *
 * (Formula is: 59*n+31 bytes, for degree n = 2^logn)
 *
 * An undersized temporary area triggers an error (returned value is zero).
 */
size_t fndsa_sign_temp(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	void *sig, size_t max_sig_len,
	void *tmp, size_t tmp_len);

/*
 * Similar to fndsa_sign_seeded(), but with an explicitly provided temporary
 * area as in fndsa_sign_temp().
 */
size_t fndsa_sign_seeded_temp(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	const void *seed, size_t seed_len,
	void *sig, size_t max_sig_len,
	void *tmp, size_t tmp_len);

/*
 * The fndsa_sign_*() functions declared above require the signing key
 * degree to be secure (512 or 1024). The fndsa_sign_weak_*() functions
 * below are similar, but require the signing key degree to be weak
 * (4 to 256). These functions are meant for tests and research only.
 */
size_t fndsa_sign_weak(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	void *sig, size_t max_sig_len);
size_t fndsa_sign_weak_seeded(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	const void *seed, size_t seed_len,
	void *sig, size_t max_sig_len);
size_t fndsa_sign_weak_temp(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	void *sig, size_t max_sig_len,
	void *tmp, size_t tmp_len);
size_t fndsa_sign_weak_seeded_temp(const void *sign_key, size_t sign_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	const void *seed, size_t seed_len,
	void *sig, size_t max_sig_len,
	void *tmp, size_t tmp_len);

/* TODO: add an API for deriving the public key from the private key?
   The code is mostly already there. */

/*
 * Verify a signature.
 *    sig, sig_len             signature
 *    vrfy_key, vrfy_key_len   verifying key (encoded)
 *    ctx, ctx_len             domain separation context (at most 255 bytes)
 *    id                       identifier for the pre-hash function
 *    hv, hv_len               pre-hashed message (or raw message)
 * Returned value is 1 if the signature is valid, 0 otherwise. Decoding
 * errors on the verifying key also imply a returned value of 0.
 *
 * If id is FNDSA_HASH_ID_EXTMU, then ctx and ctx_len are ignored (and
 * can be zero), hv_len shall be exactly 64, and hv shall point to the
 * "message representative" mu of size 64 bytes (see fndsa_compute_mu()
 * for details on how to obtain that value). Otherwise, ctx, ctx_len,
 * id, hv and hv_len shall follow the rules documented in
 * fndsa_compute_mu().
 *
 * This function accepts only the standard degrees (512 or 1024).
 */
int fndsa_verify(const void *sig, size_t sig_len,
	const void *vrfy_key, size_t vrfy_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len);

/*
 * Verify a signature (weak version).
 *
 * This function is similar to fndsa_verify(), except that it accepts only
 * keys and signatures with the weak degrees (4 to 256).
 */
int fndsa_verify_weak(const void *sig, size_t sig_len,
	const void *vrfy_key, size_t vrfy_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len);

/*
 * The fndsa_verify_temp() and fndsa_verify_weak_temp() functions are
 * similar to fndsa_verify() and fndsa_verify_weak(), respectively,
 * except that temporary storage will use the provided tmp[] area (of
 * size tmp_len bytes) instead of stack buffers. These functions are
 * provided for the benefit of small embedded systems that have only
 * small stacks. Temporary area sizes are as follows:
 *
 *    logn   min tmp_len   security
 *   -----------------------------------------
 *      9       2079       standard (level I)
 *     10       4127       standard (level V)
 *
 *      2         47       none
 *      3         63       none
 *      4         95       none
 *      5        159       none
 *      6        287       none
 *      7        543       very weak
 *      8       1055       presumed weak
 *
 * (Formula is: 4*n+31 bytes, for degree n = 2^logn)
 *
 * An undersized temporary area triggers an error (returned value is zero).
 */
int fndsa_verify_temp(const void *sig, size_t sig_len,
	const void *vrfy_key, size_t vrfy_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	void *tmp, size_t tmp_len);
int fndsa_verify_weak_temp(const void *sig, size_t sig_len,
	const void *vrfy_key, size_t vrfy_key_len,
	const void *ctx, size_t ctx_len,
	const char *id, const void *hv, size_t hv_len,
	void *tmp, size_t tmp_len);

#endif
