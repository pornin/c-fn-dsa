	.syntax	unified
	.cpu	cortex-m4
	.file	"sign_fpr_cm4.s"
	.text

@ =======================================================================
@
@ FLOATING-POINT RULES
@ --------------------
@
@ We use the IEEE-754 'binary64' format with roundTiesToEven policy.
@ THESE FUNCTIONS ARE MEANT TO BE USED IN THE CONTEXT OF FALCON/FN-DSA
@ ONLY. In particular, the following assumptions are made:
@
@  - Inputs and outputs are never NaN, infinite, or denormalized. Only
@    normalized values and zeros may ever be provided as parameters
@    or returned.
@
@  - No operation overflows or underflows. Divisions are always well-defined
@    (divisor is never zero). Square roots are never called on a negative
@    value.
@
@  - Encoded exponents (bits 20 to 30 of the high word) can only have
@    a value in [547,1102] (i.e. the actual exponent is in [-476,+79],
@    and absolute values are at least 2^(-476), and less than 2^80).
@    See: https://eprint.iacr.org/2024/321
@
@ All code is designed to be constant-time (the floating-point values are
@ secret) even in situations involving zeros.
@
@ =======================================================================

@ =======================================================================
@ fpr fndsa_fpr_of32(int32_t i)
@ =======================================================================

	.align	2
	.global	fndsa_fpr_of32
	.thumb
	.thumb_func
	.type	fndsa_fpr_of32, %function
fndsa_fpr_of32:
	@ Get absolute value.
	eor	r2, r0, r0, asr #31
	sub	r2, r2, r0, asr #31

	@ Normalize to [2^31,2^32-1].
	clz	r3, r2
	lsls	r2, r3

	@ Value was in [2^n,2^(n+1)-1] with n = 31 - r3. The exponent
	@ should be n + 1023 = 1054 - r3 (but adding the mantissa will add
	@ 1 to the exponent). Exception: if the source value is zero, then
	@ the encoded exponent shall be zero.
	subw	r3, r3, #1053
	and	r3, r3, r2, asr #31

	@ Plug sign and exponent in top output word.
	and	r1, r0, #0x80000000
	sub	r1, r1, r3, lsl #20

	@ Add the mantissa. There is no rounding.
	add	r1, r1, r2, lsr #11
	lsls	r0, r2, #21

	bx	lr
	.size	fndsa_fpr_of32,.-fndsa_fpr_of32

@ =======================================================================
@ fpr fndsa_fpr_scaled(int64_t i, int sc)
@ =======================================================================

	.align	2
	.global	fndsa_fpr_scaled
	.thumb
	.thumb_func
	.type	fndsa_fpr_scaled, %function
fndsa_fpr_scaled:
	@push	{ r4, r5 }
	vmov	s0, s1, r4, r5

	@ Get sign into r5 (0 or -1) and absolute value into r0:r1
	asrs	r5, r1, #31
	eors	r0, r5
	eors	r1, r5
	smlal	r0, r1, r5, r5

	@ Normalize the result with some left-shifting to full 64-bit,
	@ adjusting the scaling (r2) accordingly.
	@ We first handle the case of a top word (r1) entirely zero.
	clz	r3, r1
	sbfx	r12, r3, #5, #1
	umlal	r0, r1, r0, r12
	add	r2, r2, r12, lsl #5

	@ Now we do the remaining shift, of up to 31 positions (if value
	@ is zero, the exponent is corrected afterwards).
	clz	r3, r1
	subs	r2, r2, r3
	movs	r4, #1
	lsls	r4, r3
	umull	r0, r12, r0, r4
	mla	r12, r4, r1, r12

	@ Normalized absolute value is now in r0:r12.
	@ If the source integer was zero, then r0:r12 = 0 at this point.
	@ Since the pre-normalized absolute value was at most 2^63-1, the
	@ lowest bit of r0 is necessarily zero.

	@ Adjust exponent. The mantissa will spill an extra 1 into the
	@ exponent.
	addw	r2, r2, #1085

	@ If source was non-zero, then msb(r12) = 1. We can thus use msb(r12)
	@ to clear the exponent if the value is zero.
	and	r2, r2, r12, asr #31

	@ Shrink mantissa to [2^52,2^53-1] with rounding.
	@ See fpr_add() for details. Since we can only guarantee that the
	@ lowest bit is 0, the method involves adding 0x7FE00000, which
	@ cannot fit in a representable constant for add; we have to
	@ use movw and a shift.
	lsls	r2, r2, #20          @ exponent
	lsls	r4, r0, #21
	lsrs	r0, r0, #11
	bfi	r4, r0, #21, #1
	movw	r1, #0x7FE0
	adds	r4, r4, r1, lsl #16
	adcs	r0, r0, r12, lsl #21
	adcs	r1, r2, r12, lsr #11
	bfi	r1, r5, #31, #1      @ sign

	@ Alternate method, kept here for future optimizations.
	@ If the least kept bit is c, and dropped bits are b10:b0, then
	@ the rounding adjustment is adding And(b10, Or(c, b9:b0)) (i.e.
	@ adding 1 if b10 is 1 _and_ at least one of c or bits b0 to b9
	@ is set).
	@lsls	r1, r2, #20          @ exponent
	@bfi	r1, r5, #31, #1      @ sign
	@movw	r4, #0x0BFF
	@ands	r4, r0
	@usat	r4, #1, r4
	@and	r4, r4, r0, lsr #10
	@add	r0, r4, r0, lsr #11
	@adds	r0, r0, r12, lsl #21
	@adcs	r1, r1, r12, lsr #11

	@pop	{ r4, r5 }
	vmov	r4, r5, s0, s1
	bx	lr
	.size	fndsa_fpr_scaled,.-fndsa_fpr_scaled

@ =======================================================================
@ fpr fndsa_fpr_add(fpr x, fpr y)
@ =======================================================================

	.align	2
	.global	fndsa_fpr_add
	.thumb
	.thumb_func
	.type	fndsa_fpr_add, %function
fndsa_fpr_add:
	@push	{ r4, r5, r6, r7 }
	vmov	s0, s1, r4, r5
	vmov	s2, s3, r6, r7

	@ Operands are in r0:r1 and r2:r3. We want to conditionally swap
	@ them, so that x (r0:r1) has the greater absolute value of the two;
	@ if both have the same absolute value and different signs, then
	@ x should be positive. This ensures that the exponent of y is not
	@ greater than that of x, and the result has the sign of x.
	@
	@ To ignore the sign bit in the comparison, we left-shift the high
	@ word of both operands by 1 bit (this does not change the order of
	@ the absolute values). To cover the case of two equal absolute
	@ values, we inject the sign of x as an initial borrow (thus, if
	@ the absolute values are equal but x is negative, then the
	@ comparison will decide that x is "lower" and do the swap). We
	@ leverage the fact that r1 cannot be 0xFFFFFFFF (it would mean that
	@ x is a NaN), and therefore subtracting the word-extended sign bit
	@ will produce the expected borrow.
	lsls	r7, r1, #1            @ Left-shift high word of x
	subs	r6, r1, r1, asr #31   @ Initial borrow if x is negative
	sbcs	r6, r0, r2            @ Sub: low words
	sbcs	r6, r7, r3, lsl #1    @ Sub: high words (with shift of y)
	sbcs	r4, r4                @ r4 is set to 0xFFFFFFFF for a swap
	uadd8	r4, r4, r4
	sel	r6, r2, r0
	sel	r7, r3, r1
	sel	r2, r0, r2
	sel	r3, r1, r3

	@ Now x is in r6:r7, and y is in r2:r3.

	@ r5[0:30] <- sign(x)
	@ r5[31] <- sign-xor
	and	r5, r3, #0x80000000
	eor	r5, r5, r7, asr #31

	@ Extract mantissa of x into r6:r7, exponent in r4.
	@ For the mantissa, we must set bit 52 to 1, except if the (encoded)
	@ exponent is zero; in the latter case, the whole value must be zero
	@ or minus zero (we do not support subnormals).
	ubfx	r4, r7, #20, #11   @ Exponent in r4 (without sign)
	usat	r1, #1, r4         @ r1 = 1 except if r4 = 0
	bfi	r7, r1, #20, #12   @ Set high mantissa bits

	@ Extract mantissa of y into r2:r3, exponent in r0.
	ubfx	r0, r3, #20, #11   @ Exponent in r0 (without sign)
	usat	r1, #1, r0         @ r1 = 1 except if r0 = 0
	bfi	r3, r1, #20, #12   @ Set high mantissa bits

	@ Scale mantissas up by three bits (i.e. multiply both by 8).
	mov	r1, #7
	lsls	r7, #3
	umlal	r6, r7, r6, r1
	lsls	r3, #3
	umlal	r2, r3, r2, r1

	@ x: exponent=r4, sign=r5[0:30], mantissa=r6:r7 (scaled up 3 bits)
	@ y: exponent=r0, sign-xor=r5[31], mantissa=r2:r3 (scaled up 3 bits)

	@ At that point, the exponent of x (in r4) is larger than that
	@ of y (in r0). The difference is the amount of shifting that
	@ should be done on y. We saturate the shift amount at 63: since
	@ the scaled y mantissa fits on 56 bits, the shifted value would
	@ be zero anyway.
	@ We won't need y's exponent beyond that point, so we store that
	@ shift count in r0.
	subs	r0, r4, r0
	usat	r0, #6, r0

	@ Shift right r2:r3 by r0 bits (with result in r3:r0). The
	@ shift count is in the 0..63 range. r12 will be non-zero if and
	@ only if some non-zero bits were dropped.

	@ If r0 >= 32, then right-shift by 32 bits; r12 is set to the
	@ dropped bits (or 0 if r0 < 32).
	@ Since r2 is a multiple of 8 at this point, we can right-shift r12.
	sbfx	r1, r0, #5, #1
	and	r12, r1, r2, lsr #1
	bic	r2, r2, r1
	umlal	r3, r2, r3, r1
	@ Right-shift by r0 mod 32 bits; dropped bits (from r3) are
	@ accumulated into r12 (with OR).
	and	r0, r0, #31
	mov	r1, #0xFFFFFFFF
	lsr	r1, r0            @ r1 <- 2^(32-sc) - 1
	eors	r0, r0
	umlal	r3, r0, r3, r1
	umlal	r2, r3, r2, r1    @ output r2 is necessarily even
	orrs	r12, r12, r2, lsr #1

	@ If r12 is non-zero then some non-zero bit was dropped and the
	@ low bit of r3 must be forced to 1 ('sticky bit'). We know that
	@ msb(r12) is 0, hence we can use usat.
	usat	r2, #1, r12
	orrs	r3, r2

	@ x: exponent=r4, sign=r5[0:30], mantissa=r6:r7 (scaled up 3 bits)
	@ y: sign-xor=r5[31], value=r3:r0 (scaled to same exponent as x)

	@ If x and y have the same sign (r5[31] = 0), then we add r3:r0 to
	@ r6:r7. Otherwise (r5[31] = 1), we subtract r3:r0 from r6:r7. Both
	@ values are less than 2^56, and output cannot be negative.
	movs	r1, #1               @ r1 = 1 will be reused later on
	orr	r2, r1, r5, asr #31  @ r2 = 1 (add) or -1 (subtract)
	add	r0, r0, r3, lsr #31
	smlal	r6, r7, r3, r2
	mla	r7, r0, r2, r7

	@ result: exponent=r4, sign=r5[0:30], mantissa=r6:r7 (scaled up 3 bits)
	@ Value in r6:r7 is necessarily less than 2^57.
	@ r1 = 1

	@ Normalize the result with some left-shifting to full 64-bit,
	@ adjusting the exponent (r4) accordingly.
	@ We first handle the case of a top word (r7) entirely zero.
	clz	r2, r7
	sbfx	r0, r2, #5, #1
	umlal	r6, r7, r6, r0
	add	r4, r4, r0, lsl #5

	@ Now we do the remaining shift, of up to 31 positions (if mantissa
	@ is zero, the exponent is corrected afterwards).
	clz	r2, r7
	subs	r4, r4, r2
	lsls	r1, r2             @ We still have r1 = 1
	umull	r6, r12, r6, r1
	mla	r12, r1, r7, r12

	@ Normalized mantissa is now in r6:r12
	@ Since the mantissa was at most 57-bit pre-normalization, the low
	@ 7 bits of r6 must be zero.

	@ The exponent of x was in r4. The left-shift operation has
	@ subtracted some value from it, 8 in case the result has the
	@ same exponent as x. However, the high bit of the mantissa will
	@ add 1 to the exponent, so we only add back 7 (the exponent is
	@ added in because rounding might have produced a carry, which
	@ should then spill into the exponent).
	adds	r4, #7

	@ If the new mantissa is non-zero, then its bit 63 is non-zero
	@ (thanks to the normalizing shift). Otherwise, that bit is
	@ zero, and we should then set the exponent to zero as well.
	ands	r4, r4, r12, asr #31

	@ We have a 64-bit value which we must shrink down to 53 bits, i.e.
	@ removing the low 11 bits. Rounding must be applied. The low 12
	@ bits of r6 (in high-to-low order) are:
	@    b4 b3 b2 b1 b0 0000000
	@ (as mentioned earlier, the lowest 7 bits must be zero)
	@ After a strict right shift, b4 is the lowest bit. Rounding will
	@ add +1 to the value if and only if:
	@   - b4 = 0 and b3:b2:b1:b0 >= 1001
	@   - b4 = 1 and b3:b2:b1:b0 >= 1000
	@ Equivalently, we must add +1 after the shift if and only if:
	@   b3:b2:b1:b0:b4 + 01111 >= 100000
	lsls	r5, #31              @ sign of output is sign of x
	orr	r1, r5, r4, lsl #20  @ exponent and sign
	lsls	r3, r6, #21          @ top(r3) = b3:b2:b1:b0:00...
	lsrs	r0, r6, #11
	bfi	r3, r0, #27, #1      @ top(r3) = b3:b2:b1:b0:b4:00...
	adds	r3, r3, #0x78000000  @ add 01111 to top bits, carry is adjust
	adcs	r0, r0, r12, lsl #21
	adcs	r1, r1, r12, lsr #11

	@ Alternate rounding method: with the notations above, rounding is
	@ +1 if b3 = 1 and at least one of {b4, b2, b1, b0} is non-zero.
	@lsls	r5, #31              @ sign of output is sign of x
	@orr	r1, r5, r4, lsl #20  @ exponent and sign
	@and	r3, r6, #0x00000B80
	@usat	r3, #1, r3
	@and	r3, r3, r6, lsr #10
	@add	r0, r3, r6, lsr #11
	@adds	r0, r0, r12, lsl #21
	@adcs	r1, r1, r12, lsr #11

	@ If the mantissa in r6:r7 was zero, then r0:r1 contains zero at
	@ this point, and the exponent r4 was cleared before, so there is
	@ not need for further correcting actions.

	@pop	{ r4, r5, r6, r7 }
	vmov	r4, r5, s0, s1
	vmov	r6, r7, s2, s3
	bx	lr
	.size	fndsa_fpr_add,.-fndsa_fpr_add

@ =======================================================================
@ fpr*2 fndsa_fpr_add_sub(fpr x, fpr y)
@ This function returns two 64-bit values: x+y in r0:r1, and x-y in r2:r3
@
@ This does not follow the AAPCS, hence the caller must be custom (inline)
@ assembly that specifies clobbers and dispatches the two results
@ appropriately.
@ Clobbers: r4, r5, r6, r7, r8, r10, r11, r12, r14, flags
@ =======================================================================

	.align	2
	.global	fndsa_fpr_add_sub
	.thumb
	.thumb_func
	.type	fndsa_fpr_add_sub, %function
fndsa_fpr_add_sub:
	@ Operands are in r0:r1 and r2:r3. We want to conditionally swap
	@ them, so that x (r0:r1) has the greater absolute value of the two;
	@ if both have the same absolute value and different signs, then
	@ x should be positive. This ensures that the exponent of y is not
	@ greater than that of x, and the result of the addition has the
	@ sign of x. We must still remember whether a swap occurred, because
	@ in that case the subtraction will compute y-x instead of x-y,
	@ and we will have to negate the second output.
	@
	@ Signs for zeros: for any z, z + (-z) and z - z should be +0,
	@ never -0. The exact process is:
	@
	@     swap <- false
	@     if abs(x) < abs(y):
	@         swap <- true
	@     elif abs(x) == abs(y):
	@         if is_neg(x):
	@             swap <- true
        @     if swap:
	@         (x, y) <- (y, x)
	@     a <- abs(x) + abs(y)
	@     b <- abs(x) - abs(y)
	@     sign(a) <- sign(x)
	@     if swap:
	@         sign(b) <- sign(x)
	@     else:
	@         sign(b) <- sign(-x)
	@
	@ Indeed, if abs(x) = abs(y):
	@   x  y  x+y  x-y
	@   +  +   +    +    no swap
	@   +  -   +    +    no swap
	@   -  +   +    -    swap
	@   -  -   -    +    swap
	@
	@ To ignore the sign bit in the comparison, we left-shift the high
	@ word of both operands by 1 bit (this does not change the order of
	@ the absolute values). To cover the case of two equal absolute
	@ values, we inject the sign of x as an initial borrow (thus, if
	@ the absolute values are equal but x is negative, then the
	@ comparison will decide that x is "lower" and do the swap). We
	@ leverage the fact that r1 cannot be 0xFFFFFFFF (it would mean that
	@ x is a NaN), and therefore subtracting the word-extended sign bit
	@ will produce the expected borrow.
	lsls	r7, r1, #1            @ Left-shift high word of x
	subs	r6, r1, r1, asr #31   @ Initial borrow if x is negative
	sbcs	r6, r0, r2            @ Sub: low words
	sbcs	r6, r7, r3, lsl #1    @ Sub: high words (with shift of y)
	sbc	r12, r12              @ r12 is set to 0xFFFFFFFF for a swap
	uadd8	r4, r12, r12
	sel	r6, r2, r0
	sel	r7, r3, r1
	sel	r2, r0, r2
	sel	r5, r1, r3

	@ Now x is in r6:r7, and y is in r2:r5.

	@ Extract mantissa of x into r6:r7, exponent in r4, sign in r5.
	@ For the mantissa, we must set bit 52 to 1, except if the (encoded)
	@ exponent is zero; in the latter case, the whole value must be zero
	@ or minus zero (we do not support subnormals).
	asrs	r3, r7, #31        @ Sign bit (extended to whole word)
	ubfx	r4, r7, #20, #11   @ Exponent in r4 (without sign)
	usat	r8, #1, r4         @ r8 = 1 except if r4 = 0
	bfi	r7, r8, #20, #12   @ Set high mantissa bits

	@ Extract mantissa of y into r2:r5, exponent in r0.
	@ r1 receives the xor of the signs of x and y (extended).
	eor	r1, r3, r5, asr #31
	ubfx	r0, r5, #20, #11   @ Exponent in r0 (without sign)
	usat	r8, #1, r0         @ r8 = 1 except if r0 = 0
	bfi	r5, r8, #20, #12   @ Set high mantissa bits

	@ Scale mantissas up by three bits (i.e. multiply both by 8).
	mov	r8, #7
	lsls	r7, #3
	umlal	r6, r7, r6, r8
	lsls	r5, #3
	umlal	r2, r5, r2, r8

	@ Prepare signs for the two results.
	@   r3[0..29] = 0
	@   r3[30] = sign of first result
	@   r3[31] = sign of second result
	@ First result as the sign of x. Second result has the sign of x,
	@ but reversed if a swap occurred (r12 = -1).
	eor	r3, r3, r12, lsl #31
	and	r3, r3, #0xC0000000

	@ x: exponent=r4, sign=r3[30], mantissa=r6:r7 (scaled up 3 bits)
	@ y: exponent=r0, sign-xor=r1, mantissa=r2:r5 (scaled up 3 bits)

	@ At that point, the exponent of x (in r4) is larger than that
	@ of y (in r0). The difference is the amount of shifting that
	@ should be done on y. We saturate the shift amount at 63: since
	@ the scaled y mantissa fits on 56 bits, the shifted value would
	@ be zero anyway.
	@ We won't need y's exponent beyond that point, so we store that
	@ shift count in r0.
	subs	r0, r4, r0
	usat	r0, #6, r0

	@ Shift right r2:r5 by r0 bits (with result in r5:r0). The
	@ shift count is in the 0..63 range. r12 will be non-zero if and
	@ only if some non-zero bits were dropped.

	@ If r0 >= 32, then right-shift by 32 bits; r12 is set to the
	@ dropped bits (or 0 if r0 < 32).
	@ Since r2 is a multiple of 8 at this point, we can right-shift r12.
	sbfx	r8, r0, #5, #1
	and	r12, r8, r2, lsr #1
	bic	r2, r2, r8
	umlal	r5, r2, r5, r8
	@ Right-shift by r0 mod 32 bits; dropped bits (from r5) are
	@ accumulated into r12 (with OR).
	and	r0, r0, #31
	mov	r8, #0xFFFFFFFF
	lsr	r8, r0            @ r8 <- 2^(32-sc) - 1
	eors	r0, r0
	umlal	r5, r0, r5, r8
	umlal	r2, r5, r2, r8    @ output r2 is necessarily even
	orr	r12, r12, r2, lsr #1

	@ If r12 is non-zero then some non-zero bit was dropped and the
	@ low bit of r5 must be forced to 1 ('sticky bit'). We know that
	@ msb(r12) is 0, hence we can use usat.
	usat	r2, #1, r12
	orrs	r5, r2

	@ x: exponent=r4, sign=r3[30], mantissa=r6:r7 (scaled up 3 bits)
	@ y: sign-xor=r1, value=r5:r0 (scaled to same exponent as x)

	@ If r1 = -1, then negate the second operand. This is equivalent
	@ to swapping the addition and subtraction results.
	eors	r5, r1
	eors	r0, r1
	smlal	r5, r0, r1, r1

	@ Compute the sum (into r6:r7) and the difference (into r10:r11).
	subs	r10, r6, r5
	sbcs	r11, r7, r0
	adds	r6, r6, r5
	adcs	r7, r7, r0

	@ Post-processing for first output
	@ --------------------------------

	@ result: exponent=r4, sign=r3[30], mantissa=r6:r7 (scaled up 3 bits)
	@ Value in r6:r7 is necessarily less than 2^57.

	@ Normalize the result with some left-shifting to full 64-bit,
	@ adjusting the exponent (r4) accordingly.
	@ We first handle the case of a top word (r7) entirely zero.
	clz	r2, r7
	sbfx	r0, r2, #5, #1
	umlal	r6, r7, r6, r0
	add	r8, r4, r0, lsl #5

	@ Now we do the remaining shift, of up to 31 positions (if mantissa
	@ is zero, the exponent is corrected afterwards).
	clz	r2, r7
	subs	r8, r8, r2
	movs	r5, #1            @ r5 = 1 will also be useful later on
	lsls	r1, r5, r2
	umull	r6, r12, r6, r1
	mla	r12, r1, r7, r12

	@ Normalized mantissa is now in r6:r12
	@ Since the mantissa was at most 57-bit pre-normalization, the low
	@ 7 bits of r6 must be zero.

	@ The exponent of x was in r8. The left-shift operation has
	@ subtracted some value from it, 8 in case the result has the
	@ same exponent as x. However, the high bit of the mantissa will
	@ add 1 to the exponent, so we only add back 7 (the exponent is
	@ added in because rounding might have produced a carry, which
	@ should then spill into the exponent).
	add	r8, r8, #7

	@ If the new mantissa is non-zero, then its bit 63 is non-zero
	@ (thanks to the normalizing shift). Otherwise, that bit is
	@ zero, and we should then set the exponent to zero as well.
	and	r8, r8, r12, asr #31

	@ We have a 64-bit value which we must shrink down to 53 bits, i.e.
	@ removing the low 11 bits. Rounding must be applied. The low 12
	@ bits of r6 (in high-to-low order) are:
	@    b4 b3 b2 b1 b0 0000000
	@ (as mentioned earlier, the lowest 7 bits must be zero)
	@ After a strict right shift, b4 is the lowest bit. Rounding will
	@ add +1 to the value if and only if b3 = 1 AND at least one of
	@ {b4, b2, b1, b0} is non-zero.
	mov	r7, #0x00200000
	and	r0, r6, #0x00000B80
	usat	r0, #1, r0            @ r0 <- Or(b4, b2, b1, b0)
	and	r0, r0, r6, lsr #10   @ r0 <- And(b3, Or(b4, b2, b1, b0))
	add	r0, r0, r6, lsr #11
	lsls	r1, r3, #1
	bfi	r1, r8, #20, #11
	umlal	r0, r1, r12, r7

	@ We keep r7 = 2^21 for the rest of the processing.

	@ Post-processing for second output
	@ ---------------------------------

	@ Unprocessed second output is in r10:r11

	@ result: exponent=r4, sign=r3[31], mantissa=r10:r11 (scaled up 3 bits)
	@ Value in r10:r11 is necessarily less than 2^57.
	@ r5 contains the value 1.
	@ r7 contains the value 0x00200000 (2^21).

	@ Normalize the result with some left-shifting to full 64-bit,
	@ adjusting the exponent (r4) accordingly.
	@ We first handle the case of a top word (r11) entirely zero.
	clz	r2, r11
	sbfx	r8, r2, #5, #1
	umlal	r10, r11, r10, r8
	add	r4, r4, r8, lsl #5

	@ Now we do the remaining shift, of up to 31 positions (if mantissa
	@ is zero, the exponent is corrected afterwards).
	clz	r2, r11
	subs	r4, r4, r2
	lsls	r8, r5, r2           @ We still have r5 = 1
	umull	r6, r12, r10, r8
	mla	r12, r11, r8, r12

	@ Normalized mantissa is now in r6:r12
	@ Since the mantissa was at most 57-bit pre-normalization, the low
	@ 7 bits of r6 must be zero.

	@ The exponent of x was in r4. The left-shift operation has
	@ subtracted some value from it, 8 in case the result has the
	@ same exponent as x. However, the high bit of the mantissa will
	@ add 1 to the exponent, so we only add back 7 (the exponent is
	@ added in because rounding might have produced a carry, which
	@ should then spill into the exponent).
	adds	r4, #7

	@ If the new mantissa is non-zero, then its bit 63 is non-zero
	@ (thanks to the normalizing shift). Otherwise, that bit is
	@ zero, and we should then set the exponent to zero as well.
	ands	r4, r4, r12, asr #31

	@ We have a 64-bit value which we must shrink down to 53 bits, i.e.
	@ removing the low 11 bits. Rounding must be applied. The low 12
	@ bits of r6 (in high-to-low order) are:
	@    b4 b3 b2 b1 b0 0000000
	@ (as mentioned earlier, the lowest 7 bits must be zero)
	@ After a strict right shift, b4 is the lowest bit. Rounding will
	@ add +1 to the value if and only if b3 = 1 AND at least one of
	@ {b4, b2, b1, b0} is non-zero.
	@ At this point:
	@    r7 still contains 2^21
	@    r3[31] already contains the correct sign (and r3[19:0] = 0)
	and	r2, r6, #0x00000B80
	usat	r2, #1, r2            @ r0 <- Or(b4, b2, b1, b0)
	and	r2, r2, r6, lsr #10   @ r0 <- And(b3, Or(b4, b2, b1, b0))
	add	r2, r2, r6, lsr #11
	bfi	r3, r4, #20, #11
	umlal	r2, r3, r12, r7       @ r7 still contains 2^21

	bx	lr
	.size	fndsa_fpr_add_sub,.-fndsa_fpr_add_sub

@ =======================================================================
@ fpr fndsa_fpr_mul(fpr x, fpr y)
@ =======================================================================

	.align	2
	.global	fndsa_fpr_mul
	.thumb
	.thumb_func
	.type	fndsa_fpr_mul, %function
fndsa_fpr_mul:
	@push	{ r4, r5, r6 }
	vmov	s0, s1, r4, r5
	vmov	s2, r6

	@ Get exponents into r6 and r12.
	ubfx	r6, r1, #20, #11
	ubfx	r12, r3, #20, #11

	@ Compute sign bit (into top of r5, other bits ignored).
	eor	r5, r1, r3

	@ Compute aggregate exponent (into r4).
	adds	r4, r6, r12
	sub	r4, r4, #1024

	@ If either exponent is zero, then:
	@  - The corresponding value is zero, and the result will be zero.
	@  - We leave the implicit bits unset, so that at least one of
	@    the mantissas is zero and leads to a zero product.
	@  - We will want to keep the sign bit.
	@ Otherwise:
	@  - We must set both implicit mantissa bits to 1.
	mul	r6, r6, r12       @ r6 != 0 iff both exponents are non-zero
	usat	r6, #1, r6        @ r6 = 0 or 1
	muls	r4, r6
	bfi	r1, r6, #20, #12  @ set implicit bit and clear exponent/sign
	bfi	r3, r6, #20, #12  @ set implicit bit and clear exponent/sign

	@ Plug the aggregate exponent into r5.
	bfi	r5, r4, #20, #11

	@ At this point:
	@   r0:r1   first mantissa (completed)
	@   r2:r3   second mantissa (completed)
	@   r5      output sign and exponent
	@ Other registers are free.

	@ Compute mantissa product into r6:r12:r4:r0.
	umull	r6, r12, r0, r2
	umull	r4, r0, r0, r3
	umaal	r12, r4, r1, r2
	umaal	r4, r0, r1, r3

	@ r1, r2 and r3 are free.

	@ Product is in [2^104, 2^106 - 2^54 + 1]. We right-shift it
	@ by 52 or 53 bits, into r3:r12, so that the output is in
	@ [2^52, 2^53-1]. We must keep track of dropped bits so that we
	@ may apply rounding properly.
	@ Set r3 to 1 if we need to shift by 53, or to 0 otherwise.
	@ If r3 is 1 then we must adjust the exponent.
	@ We also right-shift r5 by 20 bits to remove all the left-over
	@ ignored bits from the original XOR.
	lsrs	r3, r0, #9
	add	r5, r3, r5, lsr #20

	@ Set r2 to 2^11 (if r3 = 1) or 2^12 (if r3 = 0). We will use
	@ it to perform a left shift by 11 or 12 bits, which is the same
	@ as a right shift by 53 or 52 bits if we use the correct output
	@ registers.
	movw	r2, #0x1000
	lsrs	r2, r3
	@ r3 is now free.
	@ Do the shift. Dropped bits are r6 (entire register) and r1 (top
	@ bits, in order, rest of the register bits are zero).
	umull	r1, r3, r12, r2
	mul	r12, r0, r2
	umlal	r3, r12, r4, r2

	@ Rounding may need to add 1. The top bits of r1 are the top dropped
	@ bits; subsequent bits of r1, and all bits of r6, are dropped and
	@ should be compacted into one bit ("sticky bit"). If:
	@   a = r3[0]                (lsb of result, before rounding)
	@   b = r1[31]               (top droppped bit)
	@   c = Or_all(r1[30:0],r6)  (sticky bit)
	@ then we need to add 1 to the result if and only if:
	@   b and (a or c) = 1
	orr	r6, r6, r1, lsl #1
	clz	r2, r6                @ r2[5] <- not(c)
	orn	r2, r3, r2, lsr #5    @ r2[0] <- a or c
	and	r2, r2, r1, lsr #31   @ r2 <- b and (a or c)
	@ Apply rounding adjustment to value, plugging also sign and exponent.
	adds	r0, r3, r2
	adcs	r1, r12, r5, lsl #20

	@pop	{ r4, r5, r6 }
	vmov	r4, r5, s0, s1
	vmov	r6, s2
	bx	lr
	.size	fndsa_fpr_mul,.-fndsa_fpr_mul

@ =======================================================================
@ fpr fndsa_fpr_sqr(fpr x)
@ =======================================================================

	.align	2
	.global	fndsa_fpr_sqr
	.thumb
	.thumb_func
	.type	fndsa_fpr_sqr, %function
fndsa_fpr_sqr:
	@push	{ r4, r5 }
	vmov	s0, s1, r4, r5

	@ fpr_sqr() is a reduced version of fpr_mul():
	@  - Only one input, hence only one exponent and mantissa to extract.
	@  - Output sign is always positive (0).
	@  - With one less input, we can use fewer temporary registers.

	@ Get exponent into r12.
	ubfx	r12, r1, #20, #11

	@ If exponent is zero, then the result is zero (positive), so the
	@ aggregate exponent is also zero, and we should complete the
	@ mantissa with a 0 at bit position 52, not a 1.
	@ If the exponent is not zero, then the aggregate exponent will be
	@ 2*r12 - 1024; we compute it as r12 - 512, the doubling is done
	@ later on.
	usat	r2, #1, r12            @ r3 = 1 if r12 != 0, 0 otherwise
	sub	r12, r12, r2, lsl #9   @ r12 <- r12 - 512 (for a non-zero r12)
	bfi	r1, r2, #20, #12       @ set mantissa upper bits

	@ At this point:
	@   r0:r1   mantissa (completed)
	@   r12     aggregate exponent (to be doubled)
	@ Other registers are free.

	@ Compute mantissa square into r2:r3:r4:r5.
	umull	r2, r3, r0, r0
	umull	r4, r5, r0, r1
	umaal	r3, r4, r0, r1
	umaal	r4, r5, r1, r1

	@ r0 and r1 are free.

	@ Product is in [2^104, 2^106 - 2^54 + 1]. We right-shift it
	@ by 52 or 53 bits, into r3:r5, so that the output is in
	@ [2^52, 2^53-1]. We must keep track of dropped bits so that we
	@ may apply rounding properly.
	@ Set r0 to 1 if we need to shift by 53, or to 0 otherwise.
	@ If r0 is 1 then we must adjust the exponent. We also apply the
	@ doubling for the exponent here.
	lsrs	r0, r5, #9
	add	r12, r0, r12, lsl #1

	@ Set r1 to 2^11 (if r0 = 1) or 2^12 (if r0 = 0). We will use
	@ it to perform a left shift by 11 or 12 bits, which is the same
	@ as a right shift by 53 or 52 bits if we use the correct output
	@ registers.
	movw	r1, #0x1000
	lsrs	r1, r0
	@ r0 is now free.
	@ Do the shift. Dropped bits are r2 (entire register) and r0 (top
	@ bits, in order, rest of the register bits are zero).
	umull	r0, r3, r3, r1
	muls	r5, r1
	umlal	r3, r5, r4, r1

	@ Rounding may need to add 1. The top bits of r0 are the top dropped
	@ bits; subsequent bits of r0, and all bits of r2, are dropped and
	@ should be compacted into one bit ("sticky bit"). If:
	@   a = r3[0]                (lsb of result, before rounding)
	@   b = r0[31]               (top droppped bit)
	@   c = Or_all(r0[30:0],r2)  (sticky bit)
	@ then we need to add 1 to the result if and only if:
	@   b and (a or c) = 1
	orr	r2, r2, r0, lsl #1
	clz	r2, r2                @ r2[5] <- not(c)
	orn	r2, r3, r2, lsr #5    @ r2[0] <- a or c
	and	r2, r2, r0, lsr #31   @ r2 <- b and (a or c)
	@ Apply rounding adjustment to value, plugging also exponent.
	@ Sign is always 0 for a square.
	adds	r0, r3, r2
	adcs	r1, r5, r12, lsl #20

	@pop	{ r4, r5 }
	vmov	r4, r5, s0, s1
	bx	lr
	.size	fndsa_fpr_sqr,.-fndsa_fpr_sqr

@ =======================================================================
@ fpr fndsa_fpr_div(fpr x, fpr y)
@ =======================================================================

	.align	2
	.global	fndsa_fpr_div
	.thumb
	.thumb_func
	.type	fndsa_fpr_div, %function
fndsa_fpr_div:
	push	{ r4, r5, r6, r7, r8, r10, r11, r14 }

	@ Save high words of inputs (signs, exponents).
	vmov	s0, r1
	vmov	s1, r3

	@ Extract mantissas (assuming values are non-zero).
	@  r0:r1 <- x.m
	@  r2:r3 <- y.m
	ubfx	r1, r1, #0, #20
	ubfx	r3, r3, #0, #20
	orr	r1, r1, #0x00100000
	orr	r3, r3, #0x00100000

	@ Bit-by-bit division of the mantissas: we run it for 55 iterations
	@ then append an extra 56-th sticky bit (non-zero if the remainder
	@ is not zero at this point). Quotient goes to r10:r12.
	eor	r10, r10

	@ For divisor mantissa y.m, we prepare the following:
	@   r2:r3   y.m*2
	@   r4      hi(y.m*4)
	@   r5      hi(y.m*8)
	@   r6      hi(y.m*16)
	@   r7:r8   -(y.m*2)
	adds	r2, r2
	adcs	r3, r3
	adds	r7, r2, r2
	adcs	r4, r3, r3
	adds	r7, r7
	adcs	r5, r4, r4
	adds	r7, r7
	adcs	r6, r5, r5
	subs	r7, r10, r2
	sbcs	r8, r10, r3

	mov	r12, #15
.macro DIVIDEND_MUL16
	lsls	r1, #4
	umlal	r0, r1,	r0, r12
.endm
	mov	r14, #2

	@ Parameter sh is 1, 2, 3 or 4.
	@ DIVSTEP_SH takes current dividend in r0:r1 and assumes that it
	@ is left-shifted by sh bits compared to its theoretical value.
	@ Divisor if subtracted (if possible), yielding the next quotient
	@ bit, which is pushed into r10. After the conditional subtraction,
	@ the dividend is formally left-shifted by 1 bit, but this macro
	@ omits the shift.
.macro	DIVSTEP_SH  sh
	@ Check whether the dividend can be subtracted; we must use the
	@ properly shifted dividend to match the divisor shift.
	subs	r11, r0, r2, lsl #(\sh)
	.if (\sh) == 1
	sbcs	r11, r1, r3
	.elseif (\sh) == 2
	sbcs	r11, r1, r4
	.elseif (\sh) == 3
	sbcs	r11, r1, r5
	.else
	sbcs	r11, r1, r6
	.endif
	@ Inject next quotient bit in r10. Also extract that bit into r11,
	@ left-shifted by sh-1 bits (r7:r8 is negation of a shifted divisor).
	adcs	r10, r10
	.if (\sh) == 2
	and	r11, r14, r10, lsl #1
	.else
	and	r11, r10, #1
	.if (\sh) != 1
	lsl	r11, r11, #((\sh) - 1)
	.endif
	.endif
	@ Subtract the dividend conditionally on the quotient bit.
	umlal	r0, r1, r7, r11
	mla	r1, r8, r11, r1
.endm

	@ Four successive division steps.
.macro	DIVSTEP4
	DIVIDEND_MUL16
	DIVSTEP_SH  4
	DIVSTEP_SH  3
	DIVSTEP_SH  2
	DIVSTEP_SH  1
.endm

	@ Eight successive division steps.
.macro	DIVSTEP8
	DIVSTEP4
	DIVSTEP4
.endm

	@ First 24 iterations to get the upper 24 quotient bits.
	DIVSTEP8
	DIVSTEP8
	DIVSTEP8

	@ Save upper quotient bits.
	vmov	s2, r10

	@ 31 iterations for the next bits.
	DIVSTEP8
	DIVSTEP8
	DIVSTEP8
	DIVSTEP4
	DIVIDEND_MUL16
	DIVSTEP_SH  4
	DIVSTEP_SH  3
	DIVSTEP_SH  2

	@ Current remainder is in r0:r1 (left-shifted by 1 bit). If it is
	@ non-zero then we must set the last bit of the quotient (sticky bit).
	subs	r0, #1
	sbcs	r1, #0
	adcs	r10, r10

	@ Restore upper quotient bits into r12.
	vmov	r12, s2

	@ We have a quotient q in r10:r12, with value up to 2^56-1. It cannot
	@ be lower than 2^54, since both operands were in [2^52, 2^53-1].
	@ This is a situation similar to that of multiplication. We
	@ normalize r10:r12 to 2^54..2^55-1 (into r6:r7) with a conditional
	@ shift (low bit is sticky). r5 contains -1 if the shift was done,
	@ 0 otherwise.
	sbfx	r5, r12, #23, #1
	subs	r4, r5, #1
	rors	r4, #1
	eors	r7, r7
	umlal	r12, r7, r12, r4
	umlal	r10, r12, r10, r4
	orr	r6, r12, r10, lsr #31   @ dropped bit is sticky

	@ We recover source top words into r1 and r3. r5 contains the extra
	@ shift flag. r6:r7 is the 55-bit output mantissa. Other registers
	@ are free.
	vmov	r1, s0
	vmov	r3, s1

	@ Extract source exponents ex and ey (encoded) into r0 and r2.
	@ Also set r4 to a negative value if x = 0, or to 0 otherwise
	@ (by our assumptions, divisor y is non-zero).
	ubfx	r0, r1, #20, #11
	ubfx	r2, r3, #20, #11
	subs	r4, r0, #1

	@ Compute aggregate exponent: ex - ey + 1022 + w
	@ (where w = 1 if the conditional shift was done, 0 otherwise)
	@ But we subtract 1 because the injection of the mantissa high
	@ bit will increment the exponent by 1.
	subs	r2, r0, r2
	add	r2, r2, #1021
	subs	r2, r2, r5

	@ If dividend is zero, then clamp mantissa and aggregate exponent
	@ to zero.
	bic	r2, r2, r4, asr #31
	bic	r6, r6, r4, asr #31
	bic	r7, r7, r4, asr #31

	@ Sign is the XOR of the sign of the operands. This is true in
	@ all cases, including very small results (exponent underflow)
	@ and zeros.
	eors	r1, r3
	bfc	r1, #0, #31

	@ Plug in the exponent.
	bfi	r1, r2, #20, #11

	@ r2 and r3 are free.
	@ Shift back to the normal 53-bit mantissa, with rounding.
	@ Mantissa goes into r0:r1. r1 already contains the exponent and
	@ sign bit; we must do an addition, which will also cover the case
	@ of a carry (from rounding) spilling into the exponent.
	@ Rounding adds 1 to the shifted mantissa when the three low bits
	@ of the mantissa (before the shift) are 011, 110 or 111, i.e.
	@ exactly when: (bit0 and bit1) or (bit1 and bit2) = 1.
	and	r3, r6, r6, lsr #1
	orr	r3, r3, r3, lsr #1
	and	r0, r3, #1
	add	r0, r0, r6, lsr #2
	adds	r0, r0, r7, lsl #30
	adcs	r1, r1, r7, lsr #2

	pop	{ r4, r5, r6, r7, r8, r10, r11, pc }
	.size	fndsa_fpr_div,.-fndsa_fpr_div

@ =======================================================================
@ fpr fndsa_fpr_sqrt(fpr x)
@ =======================================================================

	.align	2
	.global	fndsa_fpr_sqrt
	.thumb
	.thumb_func
	.type	fndsa_fpr_sqrt, %function
fndsa_fpr_sqrt:
	push	{ r4, r5, r6, r7, r8, r10 }

	@ Extract exponent and mantissa. By assumption, the operand is
	@ non-negative, hence we ignore the sign bit (sign bit could be 1
	@ if the operand is minus zero). We also decode the exponent
	@ corresponding to a mantissa between 1 and 2.
	@ For now, we suppose that the source is not zero.
	@ r0:r1 <- mantissa
	@ r12 <- encoded exponent
	@ r2 <- decoded exponent
	ubfx	r12, r1, #20, #11
	sub	r2, r12, #1023
	bfc	r1, #20, #12
	orr	r1, r1, #0x00100000

	@ If the exponent is odd, then multiply mantissa by 2 and subtract 1
	@ from the exponent.
	sbfx	r3, r2, #0, #1
	and	r4, r0, r3
	and	r5, r1, r3
	adds	r0, r4
	adcs	r1, r5
	adds	r2, r3

	@ Exponent is now even, we can halve it.
	asrs	r2, #1

	@ Left-shift the mantissa so that it is in [2^61, 2^63-1]. This
	@ allows performing the first 30 iterations with some shortcuts
	@ (one-word operations).
	lsls	r1, r1, #9
	orr	r1, r1, r0, lsr #23
	lsls	r0, r0, #9

	@ r0:r1 is an integer between 1 (inclusive) and 4 (exclusive) in
	@ a fixed-point notation (53 fractional bits). We compute the
	@ square root bit by bit (54 iterations). We'll then append an
	@ extra sticky bit.
	eors	r3, r3
	eors	r5, r5

.macro	SQRTSTEP_HI  bit
	orr	r6, r5, #(1 << (\bit))
	subs	r7, r1, r6
	rrx	r3, r3
	and	r6, r6, r3, asr #31
	subs	r1, r1, r6
	lsrs	r6, r3, #31
	orr	r5, r5, r6, lsl #((\bit) + 1)
	adds	r0, r0
	adcs	r1, r1
.endm

.macro  SQRTSTEP_HI_x5  bb
	SQRTSTEP_HI	((\bb) + 4)
	SQRTSTEP_HI	((\bb) + 3)
	SQRTSTEP_HI	((\bb) + 2)
	SQRTSTEP_HI	((\bb) + 1)
	SQRTSTEP_HI	((\bb) + 0)
.endm

	SQRTSTEP_HI_x5	25
	SQRTSTEP_HI_x5	20
	SQRTSTEP_HI_x5	15
	SQRTSTEP_HI_x5	10
	SQRTSTEP_HI_x5	5
	SQRTSTEP_HI_x5	0

	@ We got top 30 bits of the result, in reverse order.
	rbit	r3, r3

	@ For the next 24 iterations, we must use two-word operations.
	@ First iteration is special because the potential bit goes into
	@ r5, not r6.
	eors	r4, r4
	eors	r6, r6

	orr	r7, r6, #(1 << 31)
	subs	r8, r0, r7
	sbcs	r10, r1, r5
	rrx	r4, r4
	and	r7, r7, r4, asr #31
	and	r8, r5, r4, asr #31
	subs	r0, r0, r7
	sbcs	r1, r1, r8
	lsrs	r7, r4, #31
	orr	r5, r5, r4, lsr #31
	adds	r0, r0
	adcs	r1, r1

.macro	SQRTSTEP_LO  bit
	orr	r7, r6, #(1 << (\bit))
	subs	r8, r0, r7
	sbcs	r10, r1, r5
	rrx	r4, r4
	and	r7, r7, r4, asr #31
	and	r8, r5, r4, asr #31
	subs	r0, r0, r7
	sbcs	r1, r1, r8
	lsrs	r7, r4, #31
	orr	r6, r6, r7, lsl #((\bit) + 1)
	adds	r0, r0
	adcs	r1, r1
.endm

.macro	SQRTSTEP_LO_x4  bb
	SQRTSTEP_LO	((\bb) + 3)
	SQRTSTEP_LO	((\bb) + 2)
	SQRTSTEP_LO	((\bb) + 1)
	SQRTSTEP_LO	((\bb) + 0)
.endm

	SQRTSTEP_LO	30
	SQRTSTEP_LO	29
	SQRTSTEP_LO	28
	SQRTSTEP_LO_x4	24
	SQRTSTEP_LO_x4	20
	SQRTSTEP_LO_x4	16
	SQRTSTEP_LO_x4	12
	SQRTSTEP_LO_x4	8

	@ Put low 24 bits in the right order.
	rbit	r4, r4

	@ We now have a 54-bit result (low 24 bits in r4, top 30 bits in r3).
	@ We need to round the value; the sticky bit is implicit (it is 1 if
	@ the remainder in r0:r1 is non-zero at this point).
	orrs	r0, r1
	rsbs	r1, r0, #0
	orrs	r0, r1       @ sticky bit is in r0[31]
	and	r0, r4, r0, lsr #31
	and	r1, r4, r4, lsr #1
	orrs	r0, r1
	ands	r0, #1       @ r0 contains the rounding adjustment
	lsrs	r1, r3, #9
	add	r0, r0, r4, lsr #1
	adds	r0, r0, r3, lsl #23
	adcs	r1, #0

	@ We have a rounded mantissa (including its top bit). We plug the
	@ exponent, which is currently in r2 in decoded format. Since the
	@ mantissa top bit is present, we encode r2 by adding 1022.
	add	r2, #1022
	add	r1, r1, r2, lsl #20

	@ We have the result, except if the source operand was zero, in
	@ which case we must clamp the value to 0. Original exponent
	@ (encoded) is still in r12.
	rsb	r3, r12, #0
	and	r0, r0, r3, asr #31
	and	r1, r1, r3, asr #31

	pop	{ r4, r5, r6, r7, r8, r10 }
	bx	lr
	.size	fndsa_fpr_sqrt,.-fndsa_fpr_sqrt
