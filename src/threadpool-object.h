#pragma once

/* Standard C headers */
#include <stddef.h>
#include <stdint.h>

/* Internal headers */
#include "threadpool-common.h"
#include "threadpool-atomics.h"

/* POSIX headers */
#if PTHREADPOOL_USE_CONDVAR || PTHREADPOOL_USE_FUTEX
#include <pthread.h>
#endif

/* Mach headers */
#if PTHREADPOOL_USE_GCD
#include <dispatch/dispatch.h>
#endif

/* Library header */
#include <pthreadpool.h>


#define THREADPOOL_COMMAND_MASK UINT32_C(0x7FFFFFFF)

enum threadpool_command {
	threadpool_command_init,
	threadpool_command_parallelize,
	threadpool_command_shutdown,
};

struct PTHREADPOOL_CACHELINE_ALIGNED thread_info {
	/**
	 * Index of the first element in the work range.
	 * Before processing a new element the owning worker thread increments this value.
	 */
	pthreadpool_atomic_size_t range_start;
	/**
	 * Index of the element after the last element of the work range.
	 * Before processing a new element the stealing worker thread decrements this value.
	 */
	pthreadpool_atomic_size_t range_end;
	/**
	 * The number of elements in the work range.
	 * Due to race conditions range_length <= range_end - range_start.
	 * The owning worker thread must decrement this value before incrementing @a range_start.
	 * The stealing worker thread must decrement this value before decrementing @a range_end.
	 */
	pthreadpool_atomic_size_t range_length;
	/**
	 * Thread number in the 0..threads_count-1 range.
	 */
	size_t thread_number;
	/**
	 * Thread pool which owns the thread.
	 */
	struct pthreadpool* threadpool;
#if PTHREADPOOL_USE_CONDVAR || PTHREADPOOL_USE_FUTEX
	/**
	 * The pthread object corresponding to the thread.
	 */
	pthread_t thread_object;
#endif
};

PTHREADPOOL_STATIC_ASSERT(sizeof(struct thread_info) % PTHREADPOOL_CACHELINE_SIZE == 0,
	"thread_info structure must occupy an integer number of cache lines (64 bytes)");

struct pthreadpool_1d_with_uarch_params {
	/**
	 * Copy of the default uarch index argument passed to a microarchitecture-aware parallelization function.
	 */
	uint32_t default_uarch_index;
	/**
	 * Copy of the max uarch index argument passed to a microarchitecture-aware parallelization function.
	 */
	uint32_t max_uarch_index;
};

struct PTHREADPOOL_CACHELINE_ALIGNED pthreadpool {
#if !PTHREADPOOL_USE_GCD
	/**
	 * The number of threads that are processing an operation.
	 */
	pthreadpool_atomic_size_t active_threads;
#endif
#if PTHREADPOOL_USE_FUTEX
	/**
	 * Indicates if there are active threads.
	 * Only two values are possible:
	 * - has_active_threads == 0 if active_threads == 0
	 * - has_active_threads == 1 if active_threads != 0
	 */
	pthreadpool_atomic_uint32_t has_active_threads;
#endif
	/**
	 * The last command submitted to the thread pool.
	 */
	pthreadpool_atomic_uint32_t command;
	/**
	 * The entry point function to call for each thread in the thread pool for parallelization tasks.
	 */
	pthreadpool_atomic_void_p thread_function;
	/**
	 * The function to call for each item.
	 */
	pthreadpool_atomic_void_p task;
	/**
	 * The first argument to the item processing function.
	 */
	pthreadpool_atomic_void_p argument;
	/**
	 * Additional parallelization parameters.
	 * These parameters are specific for each thread_function.
	 */
	union {
		struct pthreadpool_1d_with_uarch_params parallelize_1d_with_uarch;
	} params;
	/**
	 * Copy of the flags passed to a parallelization function.
	 */
	pthreadpool_atomic_uint32_t flags;
#if PTHREADPOOL_USE_CONDVAR || PTHREADPOOL_USE_FUTEX
	/**
	 * Serializes concurrent calls to @a pthreadpool_parallelize_* from different threads.
	 */
	pthread_mutex_t execution_mutex;
#endif
#if PTHREADPOOL_USE_GCD
	/**
	 * Serializes concurrent calls to @a pthreadpool_parallelize_* from different threads.
	 */
	dispatch_semaphore_t execution_semaphore;
#endif
#if PTHREADPOOL_USE_CONDVAR
	/**
	 * Guards access to the @a active_threads variable.
	 */
	pthread_mutex_t completion_mutex;
	/**
	 * Condition variable to wait until all threads complete an operation (until @a active_threads is zero).
	 */
	pthread_cond_t completion_condvar;
	/**
	 * Guards access to the @a command variable.
	 */
	pthread_mutex_t command_mutex;
	/**
	 * Condition variable to wait for change of the @a command variable.
	 */
	pthread_cond_t command_condvar;
#endif
	/**
	 * The number of threads in the thread pool. Never changes after pthreadpool_create.
	 */
	size_t threads_count;
	/**
	 * Thread information structures that immediately follow this structure.
	 */
	struct thread_info threads[];
};

PTHREADPOOL_STATIC_ASSERT(sizeof(struct pthreadpool) % PTHREADPOOL_CACHELINE_SIZE == 0,
	"pthreadpool structure must occupy an integer number of cache lines (64 bytes)");

PTHREADPOOL_INTERNAL struct pthreadpool* pthreadpool_allocate(
	size_t threads_count);

PTHREADPOOL_INTERNAL void pthreadpool_deallocate(
	struct pthreadpool* threadpool);

typedef void (*thread_function_t)(struct pthreadpool* threadpool, struct thread_info* thread);

PTHREADPOOL_INTERNAL void pthreadpool_parallelize(
	struct pthreadpool* threadpool,
	thread_function_t thread_function,
	const void* params,
	size_t params_size,
	void* task,
	void* context,
	size_t linear_range,
	uint32_t flags);
