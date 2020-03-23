#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

	static inline uint32_t pthreadpool_fetch_sub_relaxed_size_t(
		pthreadpool_atomic_size_t* address,
		uint32_t decrement)
	{
		return __c11_atomic_fetch_sub(address, decrement, __ATOMIC_RELAXED);
	}

	static inline bool pthreadpool_compare_exchange_weak_relaxed_size_t(
		pthreadpool_atomic_size_t* address,
		size_t* expected_value,
		size_t new_value)
	{
		return __c11_atomic_compare_exchange_weak(
			address, expected_value, new_value, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
	}

	static inline void pthreadpool_fence_acquire() {
		__c11_atomic_thread_fence(__ATOMIC_ACQUIRE);
	}

	static inline void pthreadpool_fence_release() {
		__c11_atomic_thread_fence(__ATOMIC_RELEASE);
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

	static inline uint32_t pthreadpool_fetch_sub_relaxed_size_t(
		pthreadpool_atomic_size_t* address,
		uint32_t decrement)
	{
		return atomic_fetch_sub_explicit(address, decrement, memory_order_relaxed);
	}

	static inline bool pthreadpool_compare_exchange_weak_relaxed_size_t(
		pthreadpool_atomic_size_t* address,
		size_t* expected_value,
		size_t new_value)
	{
		return atomic_compare_exchange_weak_explicit(
			address, expected_value, new_value, memory_order_relaxed, memory_order_relaxed);
	}

	static inline void pthreadpool_fence_acquire() {
		atomic_thread_fence(memory_order_acquire);
	}

	static inline void pthreadpool_fence_release() {
		atomic_thread_fence(memory_order_release);
	}
#endif
