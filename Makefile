# Possible options:
#   -DFNDSA_AVX2=0         disable AVX2 support
#   -DFNDSA_SSE2=0         disable SSE2 support
#   -DFNDSA_NEON=0         disable NEON support
#   -DFNDSA_RV64D=0        disable use of floating-point hardware on RISC-V
#
#   -DFNDSA_DIV_EMU=1      force integer emulation of divisions (RISC-V only)
#   -DFNDSA_SQRT_EMU=1     force integer emulation of square roots (RISC-V only)
#
# AVX2 support is compiled on x86 and x86_64 but is gated at runtime
# with a check that AVX2 is supported by the current CPU (and not
# disabled by the operating system); if AVX2 cannot be used, then the
# fallback code (normally with SSE2) is used. Thus, support of AVX2 does
# not prevent the code from running on non-AVX2 machines.
#
# SSE2 intrinsics are used if supported by the target architecture at
# compile-time (no runtime test); this is normally the case for 64-bit
# builds, since SSE2 is part of the 64-bit ABI. On 32-bit builds, this
# depends on the compiler's defaults, which are OS dependent (e.g. MSVC
# on Windows enables SSE2 by default, and so does Clang on 32-bit MacOS,
# but on 32-bit Linux this is not done by default, and you have to add
# the '-msse2' flag to the compiler to get SSE2). You can force SSE2
# usage even if not detected by setting '-DFNDSA_SSE2=1'.
#
# NEON intrisics are used if supported by the target architecture at
# compile-time (no runtime test) _and_ the build is 64-bit. Since NEON is
# part of the 64-bit ARMv8 ABI, you normally don't have to fiddle with
# that.
#
# On 64-bit RISC-V systems, the floating-point hardware is used if
# detected at compile-time (i.e. the target architecture includes the 'D'
# extension, which is part of the usual "RV64GC" package). When these
# instructions are used, the divisions and square roots may optionally
# be done with only integer computations, which is slower but possibly
# safer with regard to timing attacks.
#
# By default, this code compiles 'test_fndsa' (a test framework to
# validate that all computations are correct) and 'speed_fndsa' (speed
# benchmarks). The 'speed_fndsa' program uses the platform cycle
# counter, which _may_ be inaccessible to normal user, unless some
# specific action is performed (see comments in speed_fndsa.c for details).
# By invoking speed_fndsa with the '-tsc' parameter, the TSC is used
# instead, but the reported values are then in nanoseconds, and their
# interpretation becomes more complicated because the CPU frequency may
# vary, making the code run faster or slower depending on system load
# and temperature.

CC = clang
CFLAGS = -W -Wextra -Wundef -Wshadow -O2
LD = clang
LDFLAGS =
LIBS =

OBJ_COMM = codec.o mq.o sha3.o sysrng.o util.o
OBJ_KGEN = kgen.o kgen_fxp.o kgen_gauss.o kgen_mp31.o kgen_ntru.o kgen_poly.o kgen_zint31.o
OBJ_SIGN = sign.o sign_core.o sign_fpoly.o sign_fpr.o sign_sampler.o
OBJ_VRFY = vrfy.o
OBJ = $(OBJ_COMM) $(OBJ_KGEN) $(OBJ_SIGN) $(OBJ_VRFY)
TESTOBJ = test_fndsa.o
SPEEDOBJ = speed_fndsa.o

all: test_fndsa speed_fndsa

clean:
	-rm -f $(OBJ) $(TESTOBJ) $(SPEEDOBJ) test_fndsa speed_fndsa

test_fndsa: $(OBJ) $(TESTOBJ)
	$(LD) $(LDFLAGS) -o test_fndsa $(OBJ) $(TESTOBJ) $(LIBS)

speed_fndsa: $(OBJ) $(SPEEDOBJ)
	$(LD) $(LDFLAGS) -o speed_fndsa $(OBJ) $(SPEEDOBJ) $(LIBS)

# -----------------------------------------------------------------------

codec.o: codec.c fndsa.h inner.h
	$(CC) $(CFLAGS) -c -o codec.o codec.c

mq.o: mq.c fndsa.h inner.h
	$(CC) $(CFLAGS) -c -o mq.o mq.c

sha3.o: sha3.c fndsa.h inner.h
	$(CC) $(CFLAGS) -c -o sha3.o sha3.c

sysrng.o: sysrng.c fndsa.h inner.h
	$(CC) $(CFLAGS) -c -o sysrng.o sysrng.c

util.o: util.c fndsa.h inner.h
	$(CC) $(CFLAGS) -c -o util.o util.c

kgen.o: kgen.c fndsa.h kgen_inner.h inner.h
	$(CC) $(CFLAGS) -c -o kgen.o kgen.c

kgen_fxp.o: kgen_fxp.c fndsa.h kgen_inner.h inner.h
	$(CC) $(CFLAGS) -c -o kgen_fxp.o kgen_fxp.c

kgen_gauss.o: kgen_gauss.c fndsa.h kgen_inner.h inner.h
	$(CC) $(CFLAGS) -c -o kgen_gauss.o kgen_gauss.c

kgen_mp31.o: kgen_mp31.c fndsa.h kgen_inner.h inner.h
	$(CC) $(CFLAGS) -c -o kgen_mp31.o kgen_mp31.c

kgen_ntru.o: kgen_ntru.c fndsa.h kgen_inner.h inner.h
	$(CC) $(CFLAGS) -c -o kgen_ntru.o kgen_ntru.c

kgen_poly.o: kgen_poly.c fndsa.h kgen_inner.h inner.h
	$(CC) $(CFLAGS) -c -o kgen_poly.o kgen_poly.c

kgen_zint31.o: kgen_zint31.c fndsa.h kgen_inner.h inner.h
	$(CC) $(CFLAGS) -c -o kgen_zint31.o kgen_zint31.c

sign.o: sign.c fndsa.h sign_inner.h inner.h
	$(CC) $(CFLAGS) -c -o sign.o sign.c

sign_core.o: sign_core.c fndsa.h sign_inner.h inner.h
	$(CC) $(CFLAGS) -c -o sign_core.o sign_core.c

sign_fpoly.o: sign_fpoly.c fndsa.h sign_inner.h inner.h
	$(CC) $(CFLAGS) -c -o sign_fpoly.o sign_fpoly.c

sign_fpr.o: sign_fpr.c fndsa.h sign_inner.h inner.h
	$(CC) $(CFLAGS) -c -o sign_fpr.o sign_fpr.c

sign_sampler.o: sign_sampler.c fndsa.h sign_inner.h inner.h
	$(CC) $(CFLAGS) -c -o sign_sampler.o sign_sampler.c

vrfy.o: vrfy.c fndsa.h inner.h
	$(CC) $(CFLAGS) -c -o vrfy.o vrfy.c

test_fndsa.o: test_fndsa.c fndsa.h inner.h kgen_inner.h sign_inner.h
	$(CC) $(CFLAGS) -c -o test_fndsa.o test_fndsa.c

speed_fndsa.o: speed_fndsa.c fndsa.h inner.h
	$(CC) $(CFLAGS) -c -o speed_fndsa.o speed_fndsa.c
