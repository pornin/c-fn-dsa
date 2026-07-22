	.syntax	unified
	.cpu	cortex-m4
	.file	"mq_cm4.s"
	.text

	.equ	Q, 12289
	.equ	B_INF, 840

@ =======================================================================
@ size_t fndsa_mqpoly_decode(unsigned logn, const uint8_t *f, uint16_t *h)
@ =======================================================================

	.align	2
	.global	fndsa_mqpoly_decode
	.thumb
	.thumb_func
	.type	fndsa_mqpoly_decode, %function
fndsa_mqpoly_decode:
	push	{ r4, r5, r8, r10, r11 }

	@ ASSUMPTIONS:
	@  - logn >= 2 (hence, n is a multiple of 4)
	@  - output buffer is 32-bit aligned
	@ We process input by chunks of 7 bytes, to produce 4 values.
	@ TODO: try using chunks of 28 bytes when source is aligned; it
	@ would avoid most unaligned penalties and save 1/8 of reads.

	@ r0 <- n = 2^logn 
	movw	r3, #1
	lsl	r0, r3, r0
	@ r11 <- original source pointer
	mov	r11, r1

	@ r3 <- 0x3FFF:0x3FFF
	movw	r3, 0x3FFF
	movt	r3, 0x3FFF
	@ r10 <- q:q
	movw	r10, #Q
	movt	r10, #Q
	@ r12 <- 0xFFFFFFFF
	@ If any value overflows, then bit 15 or 31 of r12 will be cleared.
	mov	r12, #0xFFFFFFFF

fndsa_mqpoly_decode__L1:
	@ Get next 7 bytes into r4 and r5:
	@   r4: bits 0 to 31
	@   r5: bits 24 to 55
	ldr	r4, [r1], #3
	ldr	r5, [r1], #4
	@ Assemble the 4 values in r4:r5 (packed 16-bit):
	@ x0: r4<0,13>  <- r4<0,13>
	@ x1: r4<16,29> <- r4<14, 27>
	@ x2: r5<0,13>  <- r5<4, 17>
	@ x3: r5<16,29> <- r5<18, 31>
	pkhbt	r4, r4, r4, lsl #2
	pkhtb	r5, r5, r5, asr #2
	and	r4, r3
	and	r5, r3, r5, lsr #2
	@ Update the overflow mask.
	usub16	r8, r4, r10
	and	r12, r12, r8
	usub16	r8, r5, r10
	and	r12, r12, r8
	@ Store the extracted values.
	strd	r4, r5, [r2], #8
	@ Loop until all values have been decoded.
	subs	r0, #4
	bne	fndsa_mqpoly_decode__L1

	@ Get output value (number of consumed bytes).
	@ Clamp it to 0 on overflow.
	sub	r0, r1, r11
	and	r12, r12, r12, lsl #16
	and	r0, r0, r12, asr #31

	pop	{ r4, r5, r8, r10, r11 }
	bx	lr
	.size	fndsa_mqpoly_decode,.-fndsa_mqpoly_decode

@ =======================================================================
@ size_t fndsa_comp_decode(unsigned logn,
@                          const uint8_t *d, size_t dlen, int16_t *s)
@ =======================================================================

	.align	2
	.global	fndsa_comp_decode
	.thumb
	.thumb_func
	.type	fndsa_comp_decode, %function
fndsa_comp_decode:
	push	{ r4, r5, r6, r7 }

	@ r0 <- n = 2^logn
	movs	r4, #1
	lsl	r0, r4, r0
	@ r2 <- upper bound for d
	adds	r2, r1

	@ r4   acc
	@ r5   acc_len
	@ Unprocessed bits are in the low bits of acc. Only the acc_len low
	@ bits may be non-zero.
	eors	r4, r4
	eors	r5, r5

fndsa_comp_decode__L1:
	@ Invariant: acc_len <= 7 (i.e. there are at most 7 unprocessed bits).

	@ Get next 8 bits.
	cmp	r1, r2
	beq	fndsa_comp_decode__Lerr
	ldrb	r6, [r1], #1
	lsls	r6, r5
	orrs	r4, r6

	@ r6 <- low 7 absolute value bits
	@ r12 <- sign (word-extended)
	sbfx	r12, r4, #0, #1
	ubfx	r6, r4, #1, #7
	lsrs	r4, #8

	@ We injected 8 bits then consumed 8 bits: acc_len is unmodified.

	@ Locate next bit of value 1. Since there should be at most six
	@ bits of value 0, only one extra byte is needed at most.
	@ Since r4 contains exactly the buffered bits (other bits are zero),
	@ we can compare it with zero. If it is zero, then we need one
	@ extra byte.
	cbnz	r4, fndsa_comp_decode__L2
	cmp	r1, r2
	beq	fndsa_comp_decode__Lerr
	ldrb	r7, [r1], #1
	lsls	r7, r5
	orrs	r4, r7
	adds	r5, #8
	cbz	r4, fndsa_comp_decode__Lerr
fndsa_comp_decode__L2:
	@ r4 is non-zero, but contains at most 15 unprocessed bits.
	@ Locate first bit set to 1 (in low-to-high order); the index will
	@ be in [0,14].
	rbit	r7, r4
	clz	r7, r7
	@ Add 128*k (with k = index of the one-bit) to the mantissa.
	@ If we get above B_INF, this is an error.
	add	r6, r6, r7, lsl #7
	cmp	r6, #B_INF
	bhi	fndsa_comp_decode__Lerr
	@ Consume k+1 bits.
	adds	r7, #1
	lsrs	r4, r7
	subs	r5, r7

	@ We have the mantissa in r6 (verified to be at most B_INF) and
	@ the sign bit in r12 (extended to the whole word). We must apply
	@ the sign, and also reject -0 (which is invalid).
	orn	r7, r6, r12
	cbz	r7, fndsa_comp_decode__Lerr
	eor	r6, r6, r12
	sub	r6, r6, r12

	@ Write value and loop.
	strh	r6, [r3], #2
	subs	r0, #1
	bne	fndsa_comp_decode__L1

	@ Check that unused bits and extra bytes are all zero.
	movs	r0, #1
	cbnz	r4, fndsa_comp_decode__Lerr
fndsa_comp_decode__L3:
	cmp	r1, r2
	beq	fndsa_comp_decode__Lexit
	ldrb	r6, [r1], #1
	cbnz	r6, fndsa_comp_decode__Lerr
	b	fndsa_comp_decode__L3
	movs	r0, #1
fndsa_comp_decode__Lexit:
	pop	{ r4, r5, r6, r7 }
	bx	lr

fndsa_comp_decode__Lerr:
	eors	r0, r0
	b	fndsa_comp_decode__Lexit
	.size	fndsa_comp_decode,.-fndsa_comp_decode
