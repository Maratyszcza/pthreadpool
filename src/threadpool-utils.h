#pragma once

#include <stdint.h>
#include <stddef.h>

/* SSE-specific headers */
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
	#include <xmmintrin.h>
#endif

/* MSVC-specific headers */
#if defined(_MSC_VER) && _MSC_VER >= 1920
	#include <intrin.h>
	#if defined(_M_IX86) || defined(_M_X64) || defined(_M_AMD64)
		#include <immintrin.h>
	#endif
#endif


struct fpu_state {
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
	uint32_t mxcsr;
#elif defined(__arm__) && defined(__ARM_FP) && (__ARM_FP != 0)
	uint32_t fpscr;
#elif defined(__aarch64__)
	uint64_t fpcr;
#else
	char unused;
#endif
};

static inline struct fpu_state get_fpu_state() {
	struct fpu_state state = { 0 };
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
	state.mxcsr = (uint32_t) _mm_getcsr();
#elif defined(__arm__) && defined(__ARM_FP) && (__ARM_FP != 0)
	__asm__ __volatile__("VMRS %[fpscr], fpscr" : [fpscr] "=r" (state.fpscr));
#elif defined(__aarch64__)
	__asm__ __volatile__("MRS %[fpcr], fpcr" : [fpcr] "=r" (state.fpcr));
#endif
	return state;
}

static inline void set_fpu_state(const struct fpu_state state) {
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
	_mm_setcsr((unsigned int) state.mxcsr);
#elif defined(__arm__) && defined(__ARM_FP) && (__ARM_FP != 0)
	__asm__ __volatile__("VMSR fpscr, %[fpscr]" : : [fpscr] "r" (state.fpscr));
#elif defined(__aarch64__)
	__asm__ __volatile__("MSR fpcr, %[fpcr]" : : [fpcr] "r" (state.fpcr));
#endif
}

static inline void disable_fpu_denormals() {
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
	_mm_setcsr(_mm_getcsr() | 0x8040);
#elif defined(__arm__) && defined(__ARM_FP) && (__ARM_FP != 0)
	uint32_t fpscr;
	__asm__ __volatile__(
			"VMRS %[fpscr], fpscr\n"
			"ORR %[fpscr], #0x1000000\n"
			"VMSR fpscr, %[fpscr]\n"
		: [fpscr] "=r" (fpscr));
#elif defined(__aarch64__)
	uint64_t fpcr;
	__asm__ __volatile__(
			"MRS %[fpcr], fpcr\n"
			"ORR %w[fpcr], %w[fpcr], 0x1000000\n"
			"ORR %w[fpcr], %w[fpcr], 0x80000\n"
			"MSR fpcr, %[fpcr]\n"
		: [fpcr] "=r" (fpcr));
#endif
}

static inline size_t modulo_decrement(size_t i, size_t n) {
	/* Wrap modulo n, if needed */
	if (i == 0) {
		i = n;
	}
	/* Decrement input variable */
	return i - 1;
}

static inline size_t divide_round_up(size_t dividend, size_t divisor) {
	if (dividend % divisor == 0) {
		return dividend / divisor;
	} else {
		return dividend / divisor + 1;
	}
}

/* Windows headers define min and max macros; undefine it here */
#ifdef min
	#undef min
#endif

static inline size_t min(size_t a, size_t b) {
	return a < b ? a : b;
}
