#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MSVC-specific headers */
#ifdef _MSC_VER
	#include <intrin.h>
	#include <immintrin.h>
#endif


#if defined(__wasm__) && defined(__EMSCRIPTEN_PTHREADS__) && defined(__clang__)
	/*
	 * Clang for WebAssembly target lacks stdatomic.h header,
	 * even though it supports the necessary low-level intrinsics.
	 * Thus, we implement pthreadpool atomic functions on top of
	 * low-level Clang-specific interfaces for this target.
	 */

	typedef _Atomic(uint32_t) pthreadpool_atomic_uint32_t;
	typedef _Atomic(size_t)   pthreadpool_atomic_size_t;
	typedef _Atomic(void*)    pthreadpool_atomic_void_p;

	static inline uint32_t pthreadpool_load_relaxed_uint32_t(
		pthreadpool_atomic_uint32_t* address)
	{
		return __c11_atomic_load(address, __ATOMIC_RELAXED);
	}

	static inline size_t pthreadpool_load_relaxed_size_t(
		pthreadpool_atomic_size_t* address)
	{
		return __c11_atomic_load(address, __ATOMIC_RELAXED);
	}

	static inline void* pthreadpool_load_relaxed_void_p(
		pthreadpool_atomic_void_p* address)
	{
		return __c11_atomic_load(address, __ATOMIC_RELAXED);
	}

	static inline void pthreadpool_store_relaxed_uint32_t(
		pthreadpool_atomic_uint32_t* address,
		uint32_t value)
	{
		__c11_atomic_store(address, value, __ATOMIC_RELAXED);
	}

	static inline void pthreadpool_store_relaxed_size_t(
		pthreadpool_atomic_size_t* address,
		size_t value)
	{
		__c11_atomic_store(address, value, __ATOMIC_RELAXED);
	}

	static inline void pthreadpool_store_relaxed_void_p(
		pthreadpool_atomic_void_p* address,
		void* value)
	{
		__c11_atomic_store(address, value, __ATOMIC_RELAXED);
	}

	static inline void pthreadpool_store_release_uint32_t(
		pthreadpool_atomic_uint32_t* address,
		uint32_t value)
	{
		__c11_atomic_store(address, value, __ATOMIC_RELEASE);
	}

	static inline void pthreadpool_store_release_size_t(
		pthreadpool_atomic_size_t* address,
		size_t value)
	{
		__c11_atomic_store(address, value, __ATOMIC_RELEASE);
	}

	static inline size_t pthreadpool_decrement_fetch_relaxed_size_t(
		pthreadpool_atomic_size_t* address)
	{
		return __c11_atomic_fetch_sub(address, 1, __ATOMIC_RELAXED) - 1;
	}

	static inline bool pthreadpool_try_decrement_relaxed_size_t(
		pthreadpool_atomic_size_t* value)
	{
		size_t actual_value = __c11_atomic_load(value, __ATOMIC_RELAXED);
		while (actual_value != 0) {
			if (__c11_atomic_compare_exchange_weak(
				value, &actual_value, actual_value - 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
			{
				return true;
			}
		}
		return false;
	}

	static inline void pthreadpool_fence_acquire() {
		__c11_atomic_thread_fence(__ATOMIC_ACQUIRE);
	}

	static inline void pthreadpool_fence_release() {
		__c11_atomic_thread_fence(__ATOMIC_RELEASE);
	}
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_AMD64))
	typedef volatile uint32_t pthreadpool_atomic_uint32_t;
	typedef volatile size_t   pthreadpool_atomic_size_t;
	typedef void *volatile    pthreadpool_atomic_void_p;

	static inline uint32_t pthreadpool_load_relaxed_uint32_t(
		pthreadpool_atomic_uint32_t* address)
	{
		return *address;
	}

	static inline size_t pthreadpool_load_relaxed_size_t(
		pthreadpool_atomic_size_t* address)
	{
		return *address;
	}

	static inline void* pthreadpool_load_relaxed_void_p(
		pthreadpool_atomic_void_p* address)
	{
		return *address;
	}

	static inline void pthreadpool_store_relaxed_uint32_t(
		pthreadpool_atomic_uint32_t* address,
		uint32_t value)
	{
		*address = value;
	}

	static inline void pthreadpool_store_relaxed_size_t(
		pthreadpool_atomic_size_t* address,
		size_t value)
	{
		*address = value;
	}

	static inline void pthreadpool_store_relaxed_void_p(
		pthreadpool_atomic_void_p* address,
		void* value)
	{
		*address = value;
	}

	static inline void pthreadpool_store_release_uint32_t(
		pthreadpool_atomic_uint32_t* address,
		uint32_t value)
	{
		/* x86-64 stores always have release semantics; use only a compiler barrier */
		_WriteBarrier();
		*address = value;
	}

	static inline void pthreadpool_store_release_size_t(
		pthreadpool_atomic_size_t* address,
		size_t value)
	{
		/* x86-64 stores always have release semantics; use only a compiler barrier */
		_WriteBarrier();
		*address = value;
	}

	static inline size_t pthreadpool_decrement_fetch_relaxed_size_t(
		pthreadpool_atomic_size_t* address)
	{
		return (size_t) _InterlockedDecrement64((__int64 volatile*) address);
	}

	static inline bool pthreadpool_try_decrement_relaxed_size_t(
		pthreadpool_atomic_size_t* value)
	{
		size_t actual_value = *value;
		while (actual_value != 0) {
			const size_t new_value = actual_value - 1;
			const size_t expected_value = actual_value;
			actual_value = _InterlockedCompareExchange64(
				(__int64 volatile*) value, (__int64) new_value, (__int64) expected_value);
			if (actual_value == expected_value) {
				return true;
			}
		}
		return false;
	}

	static inline void pthreadpool_fence_acquire() {
		_mm_lfence();
		_ReadBarrier();
	}

	static inline void pthreadpool_fence_release() {
		_WriteBarrier();
		_mm_sfence();
	}
#elif defined(_MSC_VER) && defined(_M_IX86)
	typedef volatile uint32_t pthreadpool_atomic_uint32_t;
	typedef volatile size_t   pthreadpool_atomic_size_t;
	typedef void *volatile    pthreadpool_atomic_void_p;

	static inline uint32_t pthreadpool_load_relaxed_uint32_t(
		pthreadpool_atomic_uint32_t* address)
	{
		return *address;
	}

	static inline size_t pthreadpool_load_relaxed_size_t(
		pthreadpool_atomic_size_t* address)
	{
		return *address;
	}

	static inline void* pthreadpool_load_relaxed_void_p(
		pthreadpool_atomic_void_p* address)
	{
		return *address;
	}

	static inline void pthreadpool_store_relaxed_uint32_t(
		pthreadpool_atomic_uint32_t* address,
		uint32_t value)
	{
		*address = value;
	}

	static inline void pthreadpool_store_relaxed_size_t(
		pthreadpool_atomic_size_t* address,
		size_t value)
	{
		*address = value;
	}

	static inline void pthreadpool_store_relaxed_void_p(
		pthreadpool_atomic_void_p* address,
		void* value)
	{
		*address = value;
	}

	static inline void pthreadpool_store_release_uint32_t(
		pthreadpool_atomic_uint32_t* address,
		uint32_t value)
	{
		/* x86 stores always have release semantics */
		*address = value;
	}

	static inline void pthreadpool_store_release_size_t(
		pthreadpool_atomic_size_t* address,
		size_t value)
	{
		/* x86 stores always have release semantics */
		*address = value;
	}

	static inline size_t pthreadpool_decrement_fetch_relaxed_size_t(
		pthreadpool_atomic_size_t* address)
	{
		return (size_t) _InterlockedDecrement((long volatile*) address);
	}

	static inline bool pthreadpool_try_decrement_relaxed_size_t(
		pthreadpool_atomic_size_t* value)
	{
		size_t actual_value = *value;
		while (actual_value != 0) {
			const size_t new_value = actual_value - 1;
			const size_t expected_value = actual_value;
			actual_value = _InterlockedCompareExchange(
				(long volatile*) value, (long) new_value, (long) expected_value);
			if (actual_value == expected_value) {
				return true;
			}
		}
		return false;
	}

	static inline void pthreadpool_fence_acquire() {
		_mm_lfence();
	}

	static inline void pthreadpool_fence_release() {
		_mm_sfence();
	}
#else
	#include <stdatomic.h>

	typedef _Atomic(uint32_t) pthreadpool_atomic_uint32_t;
	typedef _Atomic(size_t)   pthreadpool_atomic_size_t;
	typedef _Atomic(void*)    pthreadpool_atomic_void_p;

	static inline uint32_t pthreadpool_load_relaxed_uint32_t(
		pthreadpool_atomic_uint32_t* address)
	{
		return atomic_load_explicit(address, memory_order_relaxed);
	}

	static inline size_t pthreadpool_load_relaxed_size_t(
		pthreadpool_atomic_size_t* address)
	{
		return atomic_load_explicit(address, memory_order_relaxed);
	}

	static inline void* pthreadpool_load_relaxed_void_p(
		pthreadpool_atomic_void_p* address)
	{
		return atomic_load_explicit(address, memory_order_relaxed);
	}

	static inline void pthreadpool_store_relaxed_uint32_t(
		pthreadpool_atomic_uint32_t* address,
		uint32_t value)
	{
		atomic_store_explicit(address, value, memory_order_relaxed);
	}

	static inline void pthreadpool_store_relaxed_size_t(
		pthreadpool_atomic_size_t* address,
		size_t value)
	{
		atomic_store_explicit(address, value, memory_order_relaxed);
	}

	static inline void pthreadpool_store_relaxed_void_p(
		pthreadpool_atomic_void_p* address,
		void* value)
	{
		atomic_store_explicit(address, value, memory_order_relaxed);
	}

	static inline void pthreadpool_store_release_uint32_t(
		pthreadpool_atomic_uint32_t* address,
		uint32_t value)
	{
		atomic_store_explicit(address, value, memory_order_release);
	}

	static inline void pthreadpool_store_release_size_t(
		pthreadpool_atomic_size_t* address,
		size_t value)
	{
		atomic_store_explicit(address, value, memory_order_release);
	}

	static inline size_t pthreadpool_decrement_fetch_relaxed_size_t(
		pthreadpool_atomic_size_t* address)
	{
		return atomic_fetch_sub_explicit(address, 1, memory_order_relaxed) - 1;
	}

	static inline bool pthreadpool_try_decrement_relaxed_size_t(
		pthreadpool_atomic_size_t* value)
	{
		#if defined(__clang__) && (defined(__arm__) || defined(__aarch64__))
			size_t actual_value;
			do {
				actual_value = __builtin_arm_ldrex((const volatile size_t*) value);
				if (actual_value == 0) {
					__builtin_arm_clrex();
					return false;
				}
			} while (__builtin_arm_strex(actual_value - 1, (volatile size_t*) value) != 0);
			return true;
		#else
			size_t actual_value = pthreadpool_load_relaxed_size_t(value);
			while (actual_value != 0) {
				if (atomic_compare_exchange_weak_explicit(
					value, &actual_value, actual_value - 1, memory_order_relaxed, memory_order_relaxed))
				{
					return true;
				}
			}
			return false;
		#endif
	}

	static inline void pthreadpool_fence_acquire() {
		atomic_thread_fence(memory_order_acquire);
	}

	static inline void pthreadpool_fence_release() {
		atomic_thread_fence(memory_order_release);
	}
#endif
