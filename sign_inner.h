#ifndef FNDSA_SIGN_INNER_H__
#define FNDSA_SIGN_INNER_H__

/* ==================================================================== */
/*
 * This file includes declarations used by the signature generation code
 * only.
 */

#include "inner.h"

/* ==================================================================== */
/*
 * Floating-point values.
 *
 * The values need to follow the exact rounding rules of IEEE-754
 * ('binary64' type, with roundTiesToEven policy). Infinites, NaNs and
 * denormals are not used in the algorithm and need not be supported
 * (or can be assumed not the happen).
 *
 * Format of a 64-bit value is the following:
 *
 *  s eee...e mmm...e
 *
 * with:
 *    s = sign bit (0 = positive, 1 = negative)
 *    e = exponent (11 bits)
 *    m = mantissa (52 bits)
 *
 * If e = 0, then m should be equal to zero, and this denotes a zero (there
 * are two zeros, +0.0 and -0.0, depending on the sign bit). Values with
 * e = 0 and m != 0 would be denormals, which are not supported here.
 *
 * If 1 <= e <= 2046, then the value is:
 *   (-1)^s * (2^52 + m) * 2^(e-1075)
 *
 * Values with e = 2047 denote an infinite (m = 0) or a NaN (m != 0); neither
 * is supported here.
 */
typedef uint64_t fpr;

/* This macro returns a constant initializer for a FPR value. The
   two parameters are an integer i and exponent e, such that:
      i and e are constant expressions of types castable to int64_t
      either i = 0, or 2^52 <= abs(i) <= 2^53 - 1
      if i = 0, then e = -1075; otherwise, value is i*2^e
   IMPORTANT: this macro is ONLY for constants; it MUST NOT be used
   for non-constant expressions, especially with secret values. */
#define FPR(i, e) ((i) < 0 ? (((uint64_t)1 << 63) ^ FPR_(-(i), e)) : FPR_(i, e))
#define FPR_(i, e) \
	(((uint64_t)(int64_t)(i) & (((uint64_t)1 << 63) | 0x000FFFFFFFFFFFFF)) \
	+ ((uint64_t)(((uint32_t)(e) + 1075) & 0x7FF) << 52))

#define FPR_ZERO    FPR(0, -1075)
#define FPR_NZERO   (FPR_ZERO ^ ((uint64_t)1 << 63))
#define FPR_ONE     FPR(4503599627370496, -52)

/* Right-shift a 64-bit unsigned value by a possibly secret shift count.
   The shift count is between 0 and 63 (inclusive). On some 32-bit
   architectures (especially old 32-bit PowerPC), support of 64-bit
   shifts involves conditional jumps, i.e. leaking through timing
   measurements whether the shift was 32-bit or 64-bit, which is why
   this function is defined.

   On ARM Cortex-M4, compilers apply a branchless sequence of 8 instructions
   which leverages the fact that the shift opcodes actually use an 8-bit
   count (and not 5-bit). */
static inline uint64_t
fpr_ursh(uint64_t x, int n)
{
#if FNDSA_64 || FNDSA_ASM_CORTEXM4
	return x >> n;
#else
	x ^= (x ^ (x >> 32)) & -(uint64_t)(n >> 5);
	return x >> (n & 31);
#endif
}

/* Same as fpr_ursh, but for a signed operand. */
static inline int64_t
fpr_irsh(int64_t x, int n)
{
#if FNDSA_64 || FNDSA_ASM_CORTEXM4
	return x >> n;
#else
	x ^= (x ^ (x >> 32)) & -(int64_t)(n >> 5);
	return x >> (n & 31);
#endif
}

/* Same as fpr_ursh, but for a left shift. */
static inline uint64_t
fpr_ulsh(uint64_t x, int n)
{
#if FNDSA_64 || FNDSA_ASM_CORTEXM4
	return x << n;
#else
	x ^= (x ^ (x << 32)) & -(uint64_t)(n >> 5);
	return x << (n & 31);
#endif
}

/* Given integer i and scale sc, return i*2^sc. Source integer MUST
   be in the [-(2^63-1),+(2^63-1)] range (i.e. value -2^63 is forbidden). */
#define fpr_scaled   fndsa_fpr_scaled
fpr fpr_scaled(int64_t i, int sc);

#define fpr_of(i)   fpr_scaled(i, 0)
#if FNDSA_ASM_CORTEXM4
#define fpr_of32   fndsa_fpr_of32
fpr fpr_of32(int32_t i);
#else
#define fpr_of32   fpr_of
#endif

/* Round a floating-point value to the nearest integer (roundTiesToEven
   policy). It is assumed that the mathematical result is in
   [-(2^63-1),+(2^63-1)]. */
static inline int64_t
fpr_rint(fpr x)
{
	/* Extract the mantissa as a 63-bit integer. */
	uint64_t m = ((x << 10) | ((uint64_t)1 << 62))
		& (((uint64_t)1 << 63) - 1);
	int32_t e = 1085 - ((int32_t)(x >> 52) & 0x7FF);

	/* If a shift of more than 63 bits is needed, then simply set m
	   to zero. This also covers the case of an input equal to zero. */
	m &= (uint64_t)((int64_t)(e - 64) >> 16);
	e &= 63;

	/* m should be right-shifted by e bits.
	   To apply proper rounding, we need to get the dropped bits and
	   apply the usual "sticky bit" rule. */
	uint64_t z = fpr_ulsh(m, 63 - e);
	uint64_t y = ((z & 0x3FFFFFFFFFFFFFFF) + 0x3FFFFFFFFFFFFFFF) >> 1;
	uint64_t cc = (0xC8 >> (unsigned)((z | y) >> 61)) & 1;

	/* Do the shift + rounding. */
	m = fpr_ursh(m, e) + cc;

	/* Apply the sign to get the final output. */
	uint64_t s = (uint64_t)(*(int64_t *)&x >> 63);
	m = (m ^ s) - s;
	return *(int64_t *)&m;
}

/* Like fpr_rint(), but rounding toward -infinity. */
static inline int64_t
fpr_floor(fpr x)
{
	/* We extract the mantissa as in fpr_rint(), but then we apply the
	   sign bit to it; truncation from the integer shift will then
	   yield the proper result. */
	uint64_t m = ((x << 10) | ((uint64_t)1 << 62))
		& (((uint64_t)1 << 63) - 1);
	uint64_t s = (uint64_t)(*(int64_t *)&x >> 63);
	m = (m ^ s) - s;

	/* Get the shift count. */
	int32_t e = 1085 - ((int32_t)(x >> 52) & 0x7FF);

	/* If the shift count is 64 or more, then the value should be 0
	   or -1, depending on the sign bit. Note that if the source is
	   "minus zero" then we round to -1. We only need to saturate the
	   shift count at 63. */
	uint32_t ue = (uint32_t)e;
	ue = (ue | ((63 - ue) >> 16)) & 63;
	return fpr_irsh(*(int64_t *)&m, ue);
}

/* Like fpr_rint(), but rounding toward zero. */
static inline int64_t
fpr_trunc(fpr x)
{
	/* This is like fpr_floor(), except that we apply the sign after
	   the shift instead of before. */
	uint64_t m = ((x << 10) | ((uint64_t)1 << 62))
		& (((uint64_t)1 << 63) - 1);
	int32_t e = 1085 - ((int32_t)(x >> 52) & 0x7FF);
	uint32_t ue = (uint32_t)e;
	ue = (ue | ((63 - ue) >> 16)) & 63;
	m = fpr_ursh(m, ue);

	uint64_t s = (uint64_t)(*(int64_t *)&x >> 63);
	m = (m ^ s) - s;
	return *(int64_t *)&m;
}

/* Floating-point addition. */
#define fpr_add   fndsa_fpr_add
fpr fpr_add(fpr x, fpr y);

/* Floating-point subtraction. */
static inline fpr
fpr_sub(fpr x, fpr y)
{
	return fpr_add(x, y ^ ((uint64_t)1 << 63));
}

/* Combined addition/subtraction:
    a <- x + y
    b <- x - y  */
#if FNDSA_ASM_CORTEXM4
/* On the ARM Cortex-M4, we have a dedicated function, but since it does
   not follow the AAPCS requirements (it returns two 64-bit values), it
   uses a custom convention which requires inline assembly for invocation.
   In particular, about all registers are clobbered. */
#define FPR_ADD_SUB(a, b, x, y)   do { \
		fpr t_add_sub_x = (x); \
		fpr t_add_sub_y = (y); \
		register uint32_t t_add_sub_x0 __asm__("r0") = \
			(uint32_t)t_add_sub_x; \
		register uint32_t t_add_sub_x1 __asm__("r1") = \
			(uint32_t)(t_add_sub_x >> 32); \
		register uint32_t t_add_sub_y0 __asm__("r2") = \
			(uint32_t)t_add_sub_y; \
		register uint32_t t_add_sub_y1 __asm__("r3") = \
			(uint32_t)(t_add_sub_y >> 32); \
		__asm__( \
			"bl	fndsa_fpr_add_sub" \
			: "+r" (t_add_sub_x0), "+r" (t_add_sub_x1), \
			  "+r" (t_add_sub_y0), "+r" (t_add_sub_y1) \
			: \
			: "r4", "r5", "r6", "r7", "r8", \
			  "r10", "r11", "r12", "r14", "cc"); \
		(a) = (uint64_t)t_add_sub_x0 | ((uint64_t)t_add_sub_x1 << 32); \
		(b) = (uint64_t)t_add_sub_y0 | ((uint64_t)t_add_sub_y1 << 32); \
	} while (0)
#else
#define FPR_ADD_SUB(a, b, x, y)   do { \
		fpr t_add_sub_x = (x); \
		fpr t_add_sub_y = (y); \
		(a) = fpr_add(t_add_sub_x, t_add_sub_y); \
		(b) = fpr_sub(t_add_sub_x, t_add_sub_y); \
	} while (0)
#endif

/* Floating-point negation. */
static inline fpr
fpr_neg(fpr x)
{
	return x ^ ((uint64_t)1 << 63);
}

/* Floating-point halving. */
static inline fpr
fpr_half(fpr x)
{
	/* We just have to subtract 1 from the exponent field, but we
	   must take care to preserve zeros. If the input is a zero,
	   then the subtraction on the exponent makes a borrow that
	   spills into the sign bit, which allows us to detect that
	   occurrence. */
	uint64_t y = x - ((uint64_t)1 << 52);
	y += ((x ^ y) >> 11) & ((uint64_t)1 << 52);
	return y;
}

/* Floating-point doubling. */
static inline fpr
fpr_double(fpr x)
{
	/* We add 1 to the exponent field, except if that field was zero,
	   because the double of zero is still zero. */
	uint64_t d = ((x & 0x7FF0000000000000) + 0x7FF0000000000000) >> 11;
	return x + (d & ((uint64_t)1 << 52));
}

/* Floating-point multiplication. */
#define fpr_mul   fndsa_fpr_mul
fpr fpr_mul(fpr x, fpr y);

#if FNDSA_ASM_CORTEXM4
#define fpr_sqr   fndsa_fpr_sqr
fpr fpr_sqr(fpr x);
#else
/* Floating-point squaring. */
static inline fpr
fpr_sqr(fpr x)
{
	return fpr_mul(x, x);
}
#endif

/* Floating-point division. */
#define fpr_div   fndsa_fpr_div
fpr fpr_div(fpr x, fpr y);

/* Floating-point inversion. */
static inline fpr
fpr_inv(fpr x)
{
	return fpr_div(FPR_ONE, x);
}

/* Floating-point square root. */
#define fpr_sqrt   fndsa_fpr_sqrt
fpr fpr_sqrt(fpr x);

/* Division by 2^e. */
static inline fpr
fpr_div2e(fpr x, unsigned e)
{
	uint64_t ee = (uint64_t)e << 52;
	uint64_t y = x - ee;
	uint64_t ov = x ^ y;
	return y + (ee & (uint64_t)(*(int64_t *)&ov >> 11));
}

/* Multiplication by 2^e. */
static inline fpr
fpr_mul2e(fpr x, unsigned e)
{
	/* We add e to the exponent field, except if that field was zero,
	   because the double of zero is still zero. */
	uint64_t d = (x & 0x7FF0000000000000) - ((uint64_t)1 << 52);
	d = (uint64_t)(*(int64_t *)&d >> 12);
	return x + (((uint64_t)e << 52) & ~d);
}

/* Complex multiplication. Note that we use here the version with four
   multiplications:
     d <- (a_r*b_r - a_i*b_i) + i*(a_r*b_i + a_i*b_r)
   It is faster than trying to use three multiplications, because additions
   and subtractions have about the same cost as multiplication in practice. */
#define FPC_MUL(d_re, d_im, a_re, a_im, b_re, b_im)   do { \
		fpr fpct_a_re = (a_re), fpct_a_im = (a_im); \
		fpr fpct_b_re = (b_re), fpct_b_im = (b_im); \
		fpr fpct_d_re = fpr_sub( \
			fpr_mul(fpct_a_re, fpct_b_re), \
			fpr_mul(fpct_a_im, fpct_b_im)); \
		fpr fpct_d_im = fpr_add( \
			fpr_mul(fpct_a_re, fpct_b_im), \
			fpr_mul(fpct_a_im, fpct_b_re)); \
		(d_re) = fpct_d_re; \
		(d_im) = fpct_d_im; \
	} while (0)

/* ==================================================================== */
/*
 * Pseudo-intrinsics for RISC-V.
 *
 * RISC-V hardware with the D extension implements 64-bit floating-point
 * natively and with the rounding rules that we need, but the only API
 * we have for it in C is with the native "double" which we do not want
 * to use because C compilers tend to take unwanted shortcuts such as
 * contracting expressions (in case a fused-multiply-add is available).
 * Instead, we define here our own pseudo-intrinsics which are really
 * inline assembly chunks, that the compiler will use without doing
 * such extra optimizations.
 */

#if FNDSA_RV64D
/* We wrap the type in a structure so that any attempt at applying
   arithmetic operators on f64 values triggers a compilation error.
   Since the wrapper is trivial and all these functions should be always
   inlined, this wrapper should have no extra runtime cost. */
typedef struct { double v; } f64;

static inline f64
f64_from_raw(fpr r)
{
	f64 d;
	__asm__ ("fmv.d.x  %0, %1" : "=f" (d.v) : "r" (r));
	return d;
}

static inline fpr
f64_to_raw(f64 a)
{
	fpr r;
	__asm__ ("fmv.x.d  %0, %1" : "=r" (r) : "f" (a.v));
	return r;
}

static inline f64
f64_add(f64 a, f64 b)
{
	f64 d;
	__asm__ ("fadd.d  %0, %1, %2" : "=f" (d.v) : "f" (a.v), "f" (b.v));
	return d;
}

static inline f64
f64_sub(f64 a, f64 b)
{
	f64 d;
	__asm__ ("fsub.d  %0, %1, %2" : "=f" (d.v) : "f" (a.v), "f" (b.v));
	return d;
}

static inline f64
f64_neg(f64 a)
{
	f64 d;
	__asm__ ("fsgnjn.d  %0, %1, %1" : "=f" (d.v) : "f" (a.v));
	return d;
}

static inline f64
f64_mul(f64 a, f64 b)
{
	f64 d;
	__asm__ ("fmul.d  %0, %1, %2" : "=f" (d.v) : "f" (a.v), "f" (b.v));
	return d;
}

static inline f64
f64_sqr(f64 a)
{
	return f64_mul(a, a);
}

#if FNDSA_DIV_EMU
static inline f64
f64_div(f64 a, f64 b)
{
	return f64_from_raw(fpr_div(f64_to_raw(a), f64_to_raw(b)));
}
#else
static inline f64
f64_div(f64 a, f64 b)
{
	f64 d;
	__asm__ ("fdiv.d  %0, %1, %2" : "=f" (d.v) : "f" (a.v), "f" (b.v));
	return d;
}
#endif

static inline f64
f64_inv(f64 a)
{
	return f64_div((f64){ 1.0 }, a);
}

static inline f64
f64_half(f64 a)
{
	return f64_mul((f64){ 0.5 }, a);
}

#if FNDSA_SQRT_EMU
static inline f64
f64_sqrt(f64 a)
{
	return f64_from_raw(fpr_sqrt(f64_to_raw(a)));
}
#else
static inline f64
f64_sqrt(f64 a)
{
	f64 d;
	__asm__ ("fsqrt.d  %0, %1" : "=f" (d.v) : "f" (a.v));
	return d;
}
#endif

static inline f64
f64_of(int64_t x)
{
	f64 d;
	__asm__ ("fcvt.d.l  %0, %1" : "=f" (d.v) : "r" (x));
	return d;
}

#define f64_of32(x)   f64_of((int64_t)(x))

static inline int64_t
f64_rint(f64 a)
{
	int64_t x;
	__asm__ ("fcvt.l.d  %0, %1, rne" : "=r" (x) : "f" (a.v));
	return x;
}

static inline int64_t
f64_trunc(f64 a)
{
	int64_t x;
	__asm__ ("fcvt.l.d  %0, %1, rtz" : "=r" (x) : "f" (a.v));
	return x;
}

static inline int64_t
f64_floor(f64 a)
{
	int64_t x;
	__asm__ ("fcvt.l.d  %0, %1, rdn" : "=r" (x) : "f" (a.v));
	return x;
}
#endif

/* ==================================================================== */
/*
 * Floating-point polynomials.
 *
 * Polynomials are modulo X^n+1 and have real coefficients. When converted
 * to FFT format, they have n/2 complex coefficients, with the real and
 * imaginary parts separated (real in f[i] and imaginary in f[i+n/2]).
 *
 * Unless specified explicitly, the functions below can work with
 * polynomials in both real and FFT representations (if using several
 * operands, then they must all be in the same representation).
 */

/* Convert polynomial f from real to FFT representation (in-place). */
#define fpoly_FFT   fndsa_fpoly_FFT
void fpoly_FFT(unsigned logn, fpr *f);

/* Convert polynomial f from FFT to real representation (in-place). */
#define fpoly_iFFT   fndsa_fpoly_iFFT
void fpoly_iFFT(unsigned logn, fpr *f);

/* Set polynomial d from small polynomial f with integer coefficients. */
#define fpoly_set_small   fndsa_fpoly_set_small
void fpoly_set_small(unsigned logn, fpr *d, const int8_t *f);

/* Add polynomial b to polynomial a. */
#define fpoly_add   fndsa_fpoly_add
void fpoly_add(unsigned logn, fpr *a, const fpr *b);

/* Subtract polynomial b from polynomial a. */
#define fpoly_sub   fndsa_fpoly_sub
void fpoly_sub(unsigned logn, fpr *a, const fpr *b);

/* Negate polynomial a (in-place). */
#define fpoly_neg   fndsa_fpoly_neg
void fpoly_neg(unsigned logn, fpr *a);

/* Multiply polynomial a with polynomial b (FFT representation only). */
#define fpoly_mul_fft   fndsa_fpoly_mul_fft
void fpoly_mul_fft(unsigned logn, fpr *a, const fpr *b);

/* unused
   Multiply polynomial a with the adjoint of polynomial b (FFT
   representation only).
void fpoly_muladj_fft(unsigned logn, fpr *a, const fpr *b);
*/

/* unused
   Multiply polynomial a with its own adjoint (FFT representation only).
   Coefficients n/2 to n-1 are set to zero.
void fpoly_mulownadj_fft(unsigned logn, fpr *a);
*/

/* Multiply polynomial a with real constant x. */
#define fpoly_mulconst   fndsa_fpoly_mulconst
void fpoly_mulconst(unsigned logn, fpr *a, fpr x);

/* Perform an LDL decomposition of a self-adjoint matrix G. The matrix
   is G = [[g00, g01], [adj(g01), g11]]; g00 and g11 are self-adjoint
   polynomials. The decomposition is G = L*D*adj(L), with:
      D = [[g00, 0], [0, d11]]
      L = [[1, 0], [l10, 1]]
   The output polynomials l10 and d11 are written over g01 and g11,
   respectively. g00, g11 and d11 are self-adjoint: only their first
   n/2 coefficients are accessed. g00 is unmodified. All polynomials
   are in FFT representation. */
#define fpoly_LDL_fft   fndsa_fpoly_LDL_fft
void fpoly_LDL_fft(unsigned logn, const fpr *g00, fpr *g01, fpr *g11);

/* Split operation on a polynomial: for input polynomial f,
   half-size polynomials f0 and f1 (modulo X^(n/2)+1) are such that
   f = f0(x^2) + x*f1(x^2). All polynomials are in FFT representation. */
#define fpoly_split_fft   fndsa_fpoly_split_fft
void fpoly_split_fft(unsigned logn, fpr *f0, fpr *f1, const fpr *f);

/* Specialized version of fpoly_split_fft() when the source polynomial
   f is self-adjoint. Only the first n/2 coefficients of f are accessed.
   On output, f0 is self-adjoint (all its n/2 coefficients are set), but
   in general f1 is not self-adjoint. */
#define fpoly_split_selfadj_fft   fndsa_fpoly_split_selfadj_fft
void fpoly_split_selfadj_fft(unsigned logn, fpr *f0, fpr *f1, const fpr *f);

/* Merge operation on polynomials: for input half-size polynomials f0
   and f1 (modulo X^(n/2)+1), compute f = f0(x^2) + x*f1(x^2). All
   polynomials are in FFT representation. */
#define fpoly_merge_fft   fndsa_fpoly_merge_fft
void fpoly_merge_fft(unsigned logn, fpr *f, const fpr *f0, const fpr *f1);

/* Given matrix B = [[b00, b01], [b10, b11]], compute the Gram matrix
   G = B*adj(B) = [[g00, g01], [g10, g11]], with:
      g00 = b00*adj(b00) + b01*adj(b01)
      g01 = b00*adj(b10) + b01*adj(b11)
      g10 = b10*adj(b00) + b11*adj(b01)
      g11 = b10*adj(b10) + b11*adj(b11)
   Only g00, g01 and g11 are returned, in b00, b01 and b10, respectively.
   b11 is unmodified. All polynomials are in FFT representation. */
#define fpoly_gram_fft   fndsa_fpoly_gram_fft
void fpoly_gram_fft(unsigned logn,
	fpr *b00, fpr *b01, fpr *b10, const fpr *b11);

/* Given matrix B = [[b00, b01], [b10, b11]], compute the target vector
   [t0,t1] = (1/q)*B*[0,hm], for polynomial hm with coefficients in [0,q-1].
   Only b01 and b11 are needed. b01 and b11 are consumed. t1 may be the
   same pointer as b11. hm is in normal representation; all other
   polynomials are in FFT representation. */
#define fpoly_apply_basis   fndsa_fpoly_apply_basis
void fpoly_apply_basis(unsigned logn, fpr *t0, fpr *t1,
	fpr *b01, fpr *b11, const uint16_t *hm);

/* ==================================================================== */
/*
 * Gaussian sampling.
 *
 * The sampler follows a Gaussian distribution whose centre and standard
 * deviations are not integral, dynamically obtained, and secret. The
 * sampler state includes a PRNG, from which random bytes are obtained
 * to sample value with a given bimodal Gaussian used as source for
 * rejection sampling of the target distribution.
 */

typedef struct {
	shake_context pc;
	unsigned logn;
} sampler_state;

#if 0 /* obsolete */
/* Initialize the sampler for a given degree and seed. */
#define sampler_init   fndsa_sampler_init
void sampler_init(sampler_state *ss, unsigned logn,
	const void *seed, size_t seed_len);
#endif

/* Sample the next small integer. Parameters are:
      ss       sampler state
      mu       distribution centre
      isigma   inverse of the distribution standard deviation  */
#define sampler_next   fndsa_sampler_next
int32_t sampler_next(sampler_state *ss, fpr mu, fpr isigma);

/* Apply Fast Fourier sampling:
      ss    sampler state (initialized)
      tmp   parameters and free space (total: 7*n fpr slots):
	  t0  (n fpr slots)
	  t1  (n fpr slots)
	  g01 (n fpr slots)
          g00 (hn fpr slots)
	  g11 (hn fpr slots)
	  free space (at least 3*n fpr slots)
   Target vector is [t0, t1]. Gram matrix is [[g00, g01], [adj(g01), g11]]
   with g00 and g11 being both self-adjoint (hence using only hn slots each).
   All polynomials are in FFT representation.
   All values are consumed. On output, the sampled vector [z0,z1] is written
   over [t0,t1]. */
#define ffsamp_fft   fndsa_ffsamp_fft
void ffsamp_fft(sampler_state *ss, fpr *tmp);

/* This function is global on ARM Cortex M4 so that it can be called
   from assembly code. We define its global name here so that test code
   can override it (in test_sign.c). */
#if FNDSA_ASM_CORTEXM4
#define ffsamp_fft_deepest   fndsa_ffsamp_fft_deepest
#endif

/* ==================================================================== */
/*
 * Internal signing function.
 */

/* Internal signing function. The complete signing key (encoded for f,
   g and F, but skipping the leading header byte, and decoded for G) is
   provided, as well as the message representative (mu), the random
   seed to work on, the signature output buffer, and the temporary area.
   The signature buffer has been verified to be large enough. The
   temporary area is large enough and 32-byte aligned.

   Returned value is the signature size (in bytes), or 0 on error.
   An error may conceptually happen if the private key is incorrect/invalid.

   tmp size: 58*n bytes  */
#define sign_core   fndsa_sign_core
size_t sign_core(unsigned logn,
	const uint8_t *sign_key_fgF, const int8_t *G, const uint8_t *mu,
	const uint8_t *seed, size_t seed_len, uint8_t *sig, void *tmp);

/* ==================================================================== */

#endif
