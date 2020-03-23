/* Standard C headers */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* POSIX headers */
#include <pthread.h>
#include <unistd.h>

#ifndef PTHREADPOOL_USE_FUTEX
	#if defined(__linux__)
		#define PTHREADPOOL_USE_FUTEX 1
	#elif defined(__EMSCRIPTEN__)
		#define PTHREADPOOL_USE_FUTEX 1
	#else
		#define PTHREADPOOL_USE_FUTEX 0
	#endif
#endif

/* Futex-specific headers */
#if PTHREADPOOL_USE_FUTEX
	#if defined(__linux__)
		#include <sys/syscall.h>
		#include <linux/futex.h>

		/* Old Android NDKs do not define SYS_futex and FUTEX_PRIVATE_FLAG */
		#ifndef SYS_futex
			#define SYS_futex __NR_futex
		#endif
		#ifndef FUTEX_PRIVATE_FLAG
			#define FUTEX_PRIVATE_FLAG 128
		#endif
	#elif defined(__EMSCRIPTEN__)
		/* math.h for INFINITY constant */
		#include <math.h>

		#include <emscripten/threading.h>
	#else
		#error "Platform-specific implementation of futex_wait and futex_wake_all required"
	#endif
#endif

#ifdef _WIN32
	#define NOMINMAX
	#include <malloc.h>
	#include <sysinfoapi.h>
#endif

/* Dependencies */
#include <fxdiv.h>

/* Library header */
#include <pthreadpool.h>

/* Internal headers */
#include "threadpool-utils.h"
#include "threadpool-atomics.h"

/* Number of iterations in spin-wait loop before going into futex/mutex wait */
#define PTHREADPOOL_SPIN_WAIT_ITERATIONS 1000000

#define PTHREADPOOL_CACHELINE_SIZE 64
#define PTHREADPOOL_CACHELINE_ALIGNED __attribute__((__aligned__(PTHREADPOOL_CACHELINE_SIZE)))

#if defined(__clang__)
	#if __has_extension(c_static_assert) || __has_feature(c_static_assert)
		#define PTHREADPOOL_STATIC_ASSERT(predicate, message) _Static_assert((predicate), message)
	#else
		#define PTHREADPOOL_STATIC_ASSERT(predicate, message)
	#endif
#elif defined(__GNUC__) && ((__GNUC__ > 4) || (__GNUC__ == 4) && (__GNUC_MINOR__ >= 6))
	/* Static assert is supported by gcc >= 4.6 */
	#define PTHREADPOOL_STATIC_ASSERT(predicate, message) _Static_assert((predicate), message)
#else
	#define PTHREADPOOL_STATIC_ASSERT(predicate, message)
#endif

static inline size_t multiply_divide(size_t a, size_t b, size_t d) {
	#if defined(__SIZEOF_SIZE_T__) && (__SIZEOF_SIZE_T__ == 4)
		return (size_t) (((uint64_t) a) * ((uint64_t) b)) / ((uint64_t) d);
	#elif defined(__SIZEOF_SIZE_T__) && (__SIZEOF_SIZE_T__ == 8)
		return (size_t) (((__uint128_t) a) * ((__uint128_t) b)) / ((__uint128_t) d);
	#else
		#error "Unsupported platform"
	#endif
}

static inline size_t divide_round_up(size_t dividend, size_t divisor) {
	if (dividend % divisor == 0) {
		return dividend / divisor;
	} else {
		return dividend / divisor + 1;
	}
}

static inline size_t min(size_t a, size_t b) {
	return a < b ? a : b;
}

#if PTHREADPOOL_USE_FUTEX
	#if defined(__linux__)
		static int futex_wait(pthreadpool_atomic_uint32_t* address, uint32_t value) {
			return syscall(SYS_futex, address, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, value, NULL);
		}

		static int futex_wake_all(pthreadpool_atomic_uint32_t* address) {
			return syscall(SYS_futex, address, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, INT_MAX);
		}
	#elif defined(__EMSCRIPTEN__)
		static int futex_wait(pthreadpool_atomic_uint32_t* address, uint32_t value) {
			return emscripten_futex_wait((volatile void*) address, value, INFINITY);
		}

		static int futex_wake_all(pthreadpool_atomic_uint32_t* address) {
			return emscripten_futex_wake((volatile void*) address, INT_MAX);
		}
	#else
		#error "Platform-specific implementation of futex_wait and futex_wake_all required"
	#endif
#endif

#define THREADPOOL_COMMAND_MASK UINT32_C(0x7FFFFFFF)

enum threadpool_command {
	threadpool_command_init,
	threadpool_command_compute_1d,
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
	 * The pthread object corresponding to the thread.
	 */
	pthread_t thread_object;
	/**
	 * Condition variable used to wake up the thread.
	 * When the thread is idle, it waits on this condition variable.
	 */
	pthread_cond_t wakeup_condvar;
};

PTHREADPOOL_STATIC_ASSERT(sizeof(struct thread_info) % PTHREADPOOL_CACHELINE_SIZE == 0, "thread_info structure must occupy an integer number of cache lines (64 bytes)");

struct PTHREADPOOL_CACHELINE_ALIGNED pthreadpool {
	/**
	 * The number of threads that are processing an operation.
	 */
	pthreadpool_atomic_size_t active_threads;
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
	 * The function to call for each item.
	 */
	pthreadpool_atomic_void_p task;
	/**
	 * The first argument to the item processing function.
	 */
	pthreadpool_atomic_void_p argument;
	/**
	 * Copy of the flags passed to parallelization function.
	 */
	pthreadpool_atomic_uint32_t flags;
	/**
	 * Serializes concurrent calls to @a pthreadpool_parallelize_* from different threads.
	 */
	pthread_mutex_t execution_mutex;
#if !PTHREADPOOL_USE_FUTEX
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
	 * The number of threads in the thread pool. Never changes after initialization.
	 */
	size_t threads_count;
	/**
	 * Thread information structures that immediately follow this structure.
	 */
	struct thread_info threads[];
};

PTHREADPOOL_STATIC_ASSERT(sizeof(struct pthreadpool) % PTHREADPOOL_CACHELINE_SIZE == 0, "pthreadpool structure must occupy an integer number of cache lines (64 bytes)");

static void checkin_worker_thread(struct pthreadpool* threadpool) {
	#if PTHREADPOOL_USE_FUTEX
		if (pthreadpool_fetch_sub_relaxed_size_t(&threadpool->active_threads, 1) == 1) {
			pthreadpool_store_relaxed_uint32_t(&threadpool->has_active_threads, 0);
			futex_wake_all(&threadpool->has_active_threads);
		}
	#else
		pthread_mutex_lock(&threadpool->completion_mutex);
		if (pthreadpool_fetch_sub_relaxed_size_t(&threadpool->active_threads, 1) == 1) {
			pthread_cond_signal(&threadpool->completion_condvar);
		}
		pthread_mutex_unlock(&threadpool->completion_mutex);
	#endif
}

static void wait_worker_threads(struct pthreadpool* threadpool) {
	/* Initial check */
	#if PTHREADPOOL_USE_FUTEX
		uint32_t has_active_threads = pthreadpool_load_relaxed_uint32_t(&threadpool->has_active_threads);
		if (has_active_threads == 0) {
			return;
		}
	#else
		size_t active_threads = pthreadpool_load_relaxed_size_t(&threadpool->active_threads);
		if (active_threads == 0) {
			return;
		}
	#endif

	/* Spin-wait */
	for (uint32_t i = PTHREADPOOL_SPIN_WAIT_ITERATIONS; i != 0; i--) {
		/* This fence serves as a sleep instruction */
		pthreadpool_fence_acquire();

		#if PTHREADPOOL_USE_FUTEX
			has_active_threads = pthreadpool_load_relaxed_uint32_t(&threadpool->has_active_threads);
			if (has_active_threads == 0) {
				return;
			}
		#else
			active_threads = pthreadpool_load_relaxed_size_t(&threadpool->active_threads);
			if (active_threads == 0) {
				return;
			}
		#endif
	}

	/* Fall-back to mutex/futex wait */
	#if PTHREADPOOL_USE_FUTEX
		while ((has_active_threads = pthreadpool_load_relaxed_uint32_t(&threadpool->has_active_threads)) != 0) {
			futex_wait(&threadpool->has_active_threads, 1);
		}
	#else
		pthread_mutex_lock(&threadpool->completion_mutex);
		while (pthreadpool_load_relaxed_size_t(&threadpool->active_threads) != 0) {
			pthread_cond_wait(&threadpool->completion_condvar, &threadpool->completion_mutex);
		};
		pthread_mutex_unlock(&threadpool->completion_mutex);
	#endif
}

inline static bool atomic_decrement(pthreadpool_atomic_size_t* value) {
	size_t actual_value = pthreadpool_load_relaxed_size_t(value);
	if (actual_value == 0) {
		return false;
	}
	while (!pthreadpool_compare_exchange_weak_relaxed_size_t(value, &actual_value, actual_value - 1)) {
		if (actual_value == 0) {
			return false;
		}
	}
	return true;
}

inline static size_t modulo_decrement(uint32_t i, uint32_t n) {
	/* Wrap modulo n, if needed */
	if (i == 0) {
		i = n;
	}
	/* Decrement input variable */
	return i - 1;
}

static void thread_parallelize_1d(struct pthreadpool* threadpool, struct thread_info* thread) {
	const pthreadpool_task_1d_t task = (pthreadpool_task_1d_t) pthreadpool_load_relaxed_void_p(&threadpool->task);
	void *const argument = pthreadpool_load_relaxed_void_p(&threadpool->argument);
	/* Process thread's own range of items */
	size_t range_start = pthreadpool_load_relaxed_size_t(&thread->range_start);
	while (atomic_decrement(&thread->range_length)) {
		task(argument, range_start++);
	}

	/* There still may be other threads with work */
	const size_t thread_number = thread->thread_number;
	const size_t threads_count = threadpool->threads_count;
	for (size_t tid = modulo_decrement(thread_number, threads_count);
		tid != thread_number;
		tid = modulo_decrement(tid, threads_count))
	{
		struct thread_info* other_thread = &threadpool->threads[tid];
		while (atomic_decrement(&other_thread->range_length)) {
			const size_t item_id = pthreadpool_fetch_sub_relaxed_size_t(&other_thread->range_end, 1) - 1;
			task(argument, item_id);
		}
	}

	/* Make changes by this thread visible to other threads */
	pthreadpool_fence_release();
}

static uint32_t wait_for_new_command(
	struct pthreadpool* threadpool,
	uint32_t last_command,
	uint32_t last_flags)
{
	uint32_t command = pthreadpool_load_relaxed_uint32_t(&threadpool->command);
	if (command != last_command) {
		return command;
	}

	if ((last_flags & PTHREADPOOL_FLAG_YIELD_WORKERS) == 0) {
		/* Spin-wait loop */
		for (uint32_t i = PTHREADPOOL_SPIN_WAIT_ITERATIONS; i != 0; i--) {
			/* This fence serves as a sleep instruction */
			pthreadpool_fence_acquire();

			command = pthreadpool_load_relaxed_uint32_t(&threadpool->command);
			if (command != last_command) {
				return command;
			}
		}
	}

	/* Spin-wait disabled or timed out, fall back to mutex/futex wait */
	#if PTHREADPOOL_USE_FUTEX
		do {
			futex_wait(&threadpool->command, last_command);
			command = pthreadpool_load_relaxed_uint32_t(&threadpool->command);
		} while (command == last_command);
	#else
		/* Lock the command mutex */
		pthread_mutex_lock(&threadpool->command_mutex);
		/* Read the command */
		while ((command = pthreadpool_load_relaxed_uint32_t(&threadpool->command)) == last_command) {
			/* Wait for new command */
			pthread_cond_wait(&threadpool->command_condvar, &threadpool->command_mutex);
		}
		/* Read a new command */
		pthread_mutex_unlock(&threadpool->command_mutex);
	#endif
	return command;
}

static void* thread_main(void* arg) {
	struct thread_info* thread = (struct thread_info*) arg;
	struct pthreadpool* threadpool = ((struct pthreadpool*) (thread - thread->thread_number)) - 1;
	uint32_t last_command = threadpool_command_init;
	struct fpu_state saved_fpu_state = { 0 };
	uint32_t flags = 0;

	/* Check in */
	checkin_worker_thread(threadpool);

	/* Monitor new commands and act accordingly */
	for (;;) {
		uint32_t command = wait_for_new_command(threadpool, last_command, flags);
		pthreadpool_fence_acquire();

		flags = pthreadpool_load_relaxed_uint32_t(&threadpool->flags);

		/* Process command */
		switch (command & THREADPOOL_COMMAND_MASK) {
			case threadpool_command_compute_1d:
			{
				if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
					saved_fpu_state = get_fpu_state();
					disable_fpu_denormals();
				}
				thread_parallelize_1d(threadpool, thread);
				if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
					set_fpu_state(saved_fpu_state);
				}
				break;
			}
			case threadpool_command_shutdown:
				/* Exit immediately: the master thread is waiting on pthread_join */
				return NULL;
			case threadpool_command_init:
				/* To inhibit compiler warning */
				break;
		}
		/* Notify the master thread that we finished processing */
		checkin_worker_thread(threadpool);
		/* Update last command */
		last_command = command;
	};
}

static struct pthreadpool* pthreadpool_allocate(size_t threads_count) {
	const size_t threadpool_size = sizeof(struct pthreadpool) + threads_count * sizeof(struct thread_info);
	struct pthreadpool* threadpool = NULL;
	#if defined(__ANDROID__)
		/*
		 * Android didn't get posix_memalign until API level 17 (Android 4.2).
		 * Use (otherwise obsolete) memalign function on Android platform.
		 */
		threadpool = memalign(PTHREADPOOL_CACHELINE_SIZE, threadpool_size);
		if (threadpool == NULL) {
			return NULL;
		}
	#elif defined(_WIN32)
		threadpool = _aligned_malloc(threadpool_size, PTHREADPOOL_CACHELINE_SIZE);
		if (threadpool == NULL) {
			return NULL;
		}
	#else
		if (posix_memalign((void**) &threadpool, PTHREADPOOL_CACHELINE_SIZE, threadpool_size) != 0) {
			return NULL;
		}
	#endif
	memset(threadpool, 0, threadpool_size);
	return threadpool;
}

struct pthreadpool* pthreadpool_create(size_t threads_count) {
	if (threads_count == 0) {
		#if defined(_SC_NPROCESSORS_ONLN)
			threads_count = (size_t) sysconf(_SC_NPROCESSORS_ONLN);
			#if defined(__EMSCRIPTEN_PTHREADS__)
				/* Limit the number of threads to 8 to match link-time PTHREAD_POOL_SIZE option */
				if (threads_count >= 8) {
					threads_count = 8;
				}
			#endif
		#elif defined(_WIN32)
			SYSTEM_INFO system_info;
			ZeroMemory(&system_info, sizeof(system_info));
			GetSystemInfo(&system_info);
			threads_count = (size_t) system_info.dwNumberOfProcessors;
		#else
			#error "Unsupported platform"
		#endif
	}

	struct pthreadpool* threadpool = pthreadpool_allocate(threads_count);
	if (threadpool == NULL) {
		return NULL;
	}
	threadpool->threads_count = threads_count;
	for (size_t tid = 0; tid < threads_count; tid++) {
		threadpool->threads[tid].thread_number = tid;
	}

	/* Thread pool with a single thread computes everything on the caller thread. */
	if (threads_count > 1) {
		pthread_mutex_init(&threadpool->execution_mutex, NULL);
		#if !PTHREADPOOL_USE_FUTEX
			pthread_mutex_init(&threadpool->completion_mutex, NULL);
			pthread_cond_init(&threadpool->completion_condvar, NULL);
			pthread_mutex_init(&threadpool->command_mutex, NULL);
			pthread_cond_init(&threadpool->command_condvar, NULL);
		#endif

		#if PTHREADPOOL_USE_FUTEX
			pthreadpool_store_relaxed_uint32_t(&threadpool->has_active_threads, 1);
		#endif
		pthreadpool_store_release_size_t(&threadpool->active_threads, threads_count - 1 /* caller thread */);

		/* Caller thread serves as worker #0. Thus, we create system threads starting with worker #1. */
		for (size_t tid = 1; tid < threads_count; tid++) {
			pthread_create(&threadpool->threads[tid].thread_object, NULL, &thread_main, &threadpool->threads[tid]);
		}

		/* Wait until all threads initialize */
		wait_worker_threads(threadpool);
	}
	return threadpool;
}

size_t pthreadpool_get_threads_count(struct pthreadpool* threadpool) {
	if (threadpool == NULL) {
		return 1;
	} else {
		return threadpool->threads_count;
	}
}

void pthreadpool_parallelize_1d(
	struct pthreadpool* threadpool,
	pthreadpool_task_1d_t task,
	void* argument,
	size_t range,
	uint32_t flags)
{
	if (threadpool == NULL || threadpool->threads_count <= 1 || range <= 1) {
		/* No thread pool used: execute task sequentially on the calling thread */
		struct fpu_state saved_fpu_state = { 0 };
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			saved_fpu_state = get_fpu_state();
			disable_fpu_denormals();
		}
		for (size_t i = 0; i < range; i++) {
			task(argument, i);
		}
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			set_fpu_state(saved_fpu_state);
		}
	} else {
		/* Protect the global threadpool structures */
		pthread_mutex_lock(&threadpool->execution_mutex);

		#if !PTHREADPOOL_USE_FUTEX
			/* Lock the command variables to ensure that threads don't start processing before they observe complete command with all arguments */
			pthread_mutex_lock(&threadpool->command_mutex);
		#endif

		/* Setup global arguments */
		pthreadpool_store_relaxed_void_p(&threadpool->task, task);
		pthreadpool_store_relaxed_void_p(&threadpool->argument, argument);
		pthreadpool_store_relaxed_uint32_t(&threadpool->flags, flags);

		/* Locking of completion_mutex not needed: readers are sleeping on command_condvar */
		const size_t threads_count = threadpool->threads_count;
		pthreadpool_store_relaxed_size_t(&threadpool->active_threads, threads_count - 1 /* caller thread */);
		#if PTHREADPOOL_USE_FUTEX
			pthreadpool_store_relaxed_uint32_t(&threadpool->has_active_threads, 1);
		#endif

		/* Spread the work between threads */
		size_t range_start = 0;
		for (size_t tid = 0; tid < threads_count; tid++) {
			struct thread_info* thread = &threadpool->threads[tid];
			const size_t range_end = multiply_divide(range, tid + 1, threads_count);
			pthreadpool_store_relaxed_size_t(&thread->range_start, range_start);
			pthreadpool_store_relaxed_size_t(&thread->range_end, range_end);
			pthreadpool_store_relaxed_size_t(&thread->range_length, range_end - range_start);

			/* The next range starts where the previous ended */
			range_start = range_end;
		}

		/*
		 * Update the threadpool command.
		 * Imporantly, do it after initializing command parameters (range, task, argument)
		 * ~(threadpool->command | THREADPOOL_COMMAND_MASK) flips the bits not in command mask
		 * to ensure the unmasked command is different then the last command, because worker threads
		 * monitor for change in the unmasked command.
		 */
		const uint32_t old_command = pthreadpool_load_relaxed_uint32_t(&threadpool->command);
		const uint32_t new_command = ~(old_command | THREADPOOL_COMMAND_MASK) | threadpool_command_compute_1d;

		/*
		 * Store the command with release semantics to guarantee that if a worker thread observes
		 * the new command value, it also observes the updated command parameters.
		 *
		 * Note: release semantics is necessary even with a conditional variable, because the workers might
		 * be waiting in a spin-loop rather than the conditional variable.
		 */
		pthreadpool_store_release_uint32_t(&threadpool->command, new_command);
		#if PTHREADPOOL_USE_FUTEX
			/* Wake up the threads */
			futex_wake_all(&threadpool->command);
		#else
			/* Unlock the command variables before waking up the threads for better performance */
			pthread_mutex_unlock(&threadpool->command_mutex);

			/* Wake up the threads */
			pthread_cond_broadcast(&threadpool->command_condvar);
		#endif

		/* Save and modify FPU denormals control, if needed */
		struct fpu_state saved_fpu_state = { 0 };
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			saved_fpu_state = get_fpu_state();
			disable_fpu_denormals();
		}

		/* Do computations as worker #0 */
		thread_parallelize_1d(threadpool, &threadpool->threads[0]);

		/* Restore FPU denormals control, if needed */
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			set_fpu_state(saved_fpu_state);
		}

		/* Wait until the threads finish computation */
		wait_worker_threads(threadpool);

		/* Make changes by other threads visible to this thread */
		pthreadpool_fence_acquire();

		/* Unprotect the global threadpool structures */
		pthread_mutex_unlock(&threadpool->execution_mutex);
	}
}

struct compute_1d_tile_1d_context {
	pthreadpool_task_1d_tile_1d_t task;
	void* argument;
	size_t range;
	size_t tile;
};

static void compute_1d_tile_1d(const struct compute_1d_tile_1d_context* context, size_t linear_index) {
	const size_t tile_index = linear_index;
	const size_t index = tile_index * context->tile;
	const size_t tile = min(context->tile, context->range - index);
	context->task(context->argument, index, tile);
}

void pthreadpool_parallelize_1d_tile_1d(
	pthreadpool_t threadpool,
	pthreadpool_task_1d_tile_1d_t task,
	void* argument,
	size_t range,
	size_t tile,
	uint32_t flags)
{
	if (threadpool == NULL || threadpool->threads_count <= 1 || range <= tile) {
		/* No thread pool used: execute task sequentially on the calling thread */
		struct fpu_state saved_fpu_state = { 0 };
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			saved_fpu_state = get_fpu_state();
			disable_fpu_denormals();
		}
		for (size_t i = 0; i < range; i += tile) {
			task(argument, i, min(range - i, tile));
		}
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			set_fpu_state(saved_fpu_state);
		}
	} else {
		/* Execute in parallel on the thread pool using linearized index */
		const size_t tile_range = divide_round_up(range, tile);
		struct compute_1d_tile_1d_context context = {
			.task = task,
			.argument = argument,
			.range = range,
			.tile = tile
		};
		pthreadpool_parallelize_1d(threadpool, (pthreadpool_task_1d_t) compute_1d_tile_1d, &context, tile_range, flags);
	}
}

struct compute_2d_context {
	pthreadpool_task_2d_t task;
	void* argument;
	struct fxdiv_divisor_size_t range_j;
};

static void compute_2d(const struct compute_2d_context* context, size_t linear_index) {
	const struct fxdiv_divisor_size_t range_j = context->range_j;
	const struct fxdiv_result_size_t index = fxdiv_divide_size_t(linear_index, range_j);
	context->task(context->argument, index.quotient, index.remainder);
}

void pthreadpool_parallelize_2d(
	struct pthreadpool* threadpool,
	pthreadpool_task_2d_t task,
	void* argument,
	size_t range_i,
	size_t range_j,
	uint32_t flags)
{
	if (threadpool == NULL || threadpool->threads_count <= 1 || (range_i | range_j) <= 1) {
		/* No thread pool used: execute task sequentially on the calling thread */
		struct fpu_state saved_fpu_state = { 0 };
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			saved_fpu_state = get_fpu_state();
			disable_fpu_denormals();
		}
		for (size_t i = 0; i < range_i; i++) {
			for (size_t j = 0; j < range_j; j++) {
				task(argument, i, j);
			}
		}
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			set_fpu_state(saved_fpu_state);
		}
	} else {
		/* Execute in parallel on the thread pool using linearized index */
		struct compute_2d_context context = {
			.task = task,
			.argument = argument,
			.range_j = fxdiv_init_size_t(range_j)
		};
		pthreadpool_parallelize_1d(threadpool, (pthreadpool_task_1d_t) compute_2d, &context, range_i * range_j, flags);
	}
}

struct compute_2d_tile_1d_context {
	pthreadpool_task_2d_tile_1d_t task;
	void* argument;
	struct fxdiv_divisor_size_t tile_range_j;
	size_t range_i;
	size_t range_j;
	size_t tile_j;
};

static void compute_2d_tile_1d(const struct compute_2d_tile_1d_context* context, size_t linear_index) {
	const struct fxdiv_divisor_size_t tile_range_j = context->tile_range_j;
	const struct fxdiv_result_size_t tile_index = fxdiv_divide_size_t(linear_index, tile_range_j);
	const size_t max_tile_j = context->tile_j;
	const size_t index_i = tile_index.quotient;
	const size_t index_j = tile_index.remainder * max_tile_j;
	const size_t tile_j = min(max_tile_j, context->range_j - index_j);
	context->task(context->argument, index_i, index_j, tile_j);
}

void pthreadpool_parallelize_2d_tile_1d(
	pthreadpool_t threadpool,
	pthreadpool_task_2d_tile_1d_t task,
	void* argument,
	size_t range_i,
	size_t range_j,
	size_t tile_j,
	uint32_t flags)
{
	if (threadpool == NULL || threadpool->threads_count <= 1 || (range_i <= 1 && range_j <= tile_j)) {
		/* No thread pool used: execute task sequentially on the calling thread */
		struct fpu_state saved_fpu_state = { 0 };
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			saved_fpu_state = get_fpu_state();
			disable_fpu_denormals();
		}
		for (size_t i = 0; i < range_i; i++) {
			for (size_t j = 0; j < range_j; j += tile_j) {
				task(argument, i, j, min(range_j - j, tile_j));
			}
		}
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			set_fpu_state(saved_fpu_state);
		}
	} else {
		/* Execute in parallel on the thread pool using linearized index */
		const size_t tile_range_j = divide_round_up(range_j, tile_j);
		struct compute_2d_tile_1d_context context = {
			.task = task,
			.argument = argument,
			.tile_range_j = fxdiv_init_size_t(tile_range_j),
			.range_i = range_i,
			.range_j = range_j,
			.tile_j = tile_j
		};
		pthreadpool_parallelize_1d(threadpool, (pthreadpool_task_1d_t) compute_2d_tile_1d, &context, range_i * tile_range_j, flags);
	}
}

struct compute_2d_tile_2d_context {
	pthreadpool_task_2d_tile_2d_t task;
	void* argument;
	struct fxdiv_divisor_size_t tile_range_j;
	size_t range_i;
	size_t range_j;
	size_t tile_i;
	size_t tile_j;
};

static void compute_2d_tile_2d(const struct compute_2d_tile_2d_context* context, size_t linear_index) {
	const struct fxdiv_divisor_size_t tile_range_j = context->tile_range_j;
	const struct fxdiv_result_size_t tile_index = fxdiv_divide_size_t(linear_index, tile_range_j);
	const size_t max_tile_i = context->tile_i;
	const size_t max_tile_j = context->tile_j;
	const size_t index_i = tile_index.quotient * max_tile_i;
	const size_t index_j = tile_index.remainder * max_tile_j;
	const size_t tile_i = min(max_tile_i, context->range_i - index_i);
	const size_t tile_j = min(max_tile_j, context->range_j - index_j);
	context->task(context->argument, index_i, index_j, tile_i, tile_j);
}

void pthreadpool_parallelize_2d_tile_2d(
	pthreadpool_t threadpool,
	pthreadpool_task_2d_tile_2d_t task,
	void* argument,
	size_t range_i,
	size_t range_j,
	size_t tile_i,
	size_t tile_j,
	uint32_t flags)
{
	if (threadpool == NULL || threadpool->threads_count <= 1 || (range_i <= tile_i && range_j <= tile_j)) {
		/* No thread pool used: execute task sequentially on the calling thread */
		struct fpu_state saved_fpu_state = { 0 };
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			saved_fpu_state = get_fpu_state();
			disable_fpu_denormals();
		}
		for (size_t i = 0; i < range_i; i += tile_i) {
			for (size_t j = 0; j < range_j; j += tile_j) {
				task(argument, i, j, min(range_i - i, tile_i), min(range_j - j, tile_j));
			}
		}
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			set_fpu_state(saved_fpu_state);
		}
	} else {
		/* Execute in parallel on the thread pool using linearized index */
		const size_t tile_range_i = divide_round_up(range_i, tile_i);
		const size_t tile_range_j = divide_round_up(range_j, tile_j);
		struct compute_2d_tile_2d_context context = {
			.task = task,
			.argument = argument,
			.tile_range_j = fxdiv_init_size_t(tile_range_j),
			.range_i = range_i,
			.range_j = range_j,
			.tile_i = tile_i,
			.tile_j = tile_j
		};
		pthreadpool_parallelize_1d(threadpool, (pthreadpool_task_1d_t) compute_2d_tile_2d, &context, tile_range_i * tile_range_j, flags);
	}
}

struct compute_3d_tile_2d_context {
	pthreadpool_task_3d_tile_2d_t task;
	void* argument;
	struct fxdiv_divisor_size_t tile_range_j;
	struct fxdiv_divisor_size_t tile_range_k;
	size_t range_j;
	size_t range_k;
	size_t tile_j;
	size_t tile_k;
};

static void compute_3d_tile_2d(const struct compute_3d_tile_2d_context* context, size_t linear_index) {
	const struct fxdiv_divisor_size_t tile_range_k = context->tile_range_k;
	const struct fxdiv_result_size_t tile_index_ij_k = fxdiv_divide_size_t(linear_index, tile_range_k);
	const struct fxdiv_divisor_size_t tile_range_j = context->tile_range_j;
	const struct fxdiv_result_size_t tile_index_i_j = fxdiv_divide_size_t(tile_index_ij_k.quotient, tile_range_j);
	const size_t max_tile_j = context->tile_j;
	const size_t max_tile_k = context->tile_k;
	const size_t index_i = tile_index_i_j.quotient;
	const size_t index_j = tile_index_i_j.remainder * max_tile_j;
	const size_t index_k = tile_index_ij_k.remainder * max_tile_k;
	const size_t tile_j = min(max_tile_j, context->range_j - index_j);
	const size_t tile_k = min(max_tile_k, context->range_k - index_k);
	context->task(context->argument, index_i, index_j, index_k, tile_j, tile_k);
}

void pthreadpool_parallelize_3d_tile_2d(
	pthreadpool_t threadpool,
	pthreadpool_task_3d_tile_2d_t task,
	void* argument,
	size_t range_i,
	size_t range_j,
	size_t range_k,
	size_t tile_j,
	size_t tile_k,
	uint32_t flags)
{
	if (threadpool == NULL || threadpool->threads_count <= 1 || (range_i <= 1 && range_j <= tile_j && range_k <= tile_k)) {
		/* No thread pool used: execute task sequentially on the calling thread */
		struct fpu_state saved_fpu_state = { 0 };
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			saved_fpu_state = get_fpu_state();
			disable_fpu_denormals();
		}
		for (size_t i = 0; i < range_i; i++) {
			for (size_t j = 0; j < range_j; j += tile_j) {
				for (size_t k = 0; k < range_k; k += tile_k) {
					task(argument, i, j, k, min(range_j - j, tile_j), min(range_k - k, tile_k));
				}
			}
		}
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			set_fpu_state(saved_fpu_state);
		}
	} else {
		/* Execute in parallel on the thread pool using linearized index */
		const size_t tile_range_j = divide_round_up(range_j, tile_j);
		const size_t tile_range_k = divide_round_up(range_k, tile_k);
		struct compute_3d_tile_2d_context context = {
			.task = task,
			.argument = argument,
			.tile_range_j = fxdiv_init_size_t(tile_range_j),
			.tile_range_k = fxdiv_init_size_t(tile_range_k),
			.range_j = range_j,
			.range_k = range_k,
			.tile_j = tile_j,
			.tile_k = tile_k
		};
		pthreadpool_parallelize_1d(threadpool,
			(pthreadpool_task_1d_t) compute_3d_tile_2d, &context,
			range_i * tile_range_j * tile_range_k, flags);
	}
}

struct compute_4d_tile_2d_context {
	pthreadpool_task_4d_tile_2d_t task;
	void* argument;
	struct fxdiv_divisor_size_t tile_range_kl;
	struct fxdiv_divisor_size_t range_j;
	struct fxdiv_divisor_size_t tile_range_l;
	size_t range_k;
	size_t range_l;
	size_t tile_k;
	size_t tile_l;
};

static void compute_4d_tile_2d(const struct compute_4d_tile_2d_context* context, size_t linear_index) {
	const struct fxdiv_divisor_size_t tile_range_kl = context->tile_range_kl;
	const struct fxdiv_result_size_t tile_index_ij_kl = fxdiv_divide_size_t(linear_index, tile_range_kl);
	const struct fxdiv_divisor_size_t range_j = context->range_j;
	const struct fxdiv_result_size_t tile_index_i_j = fxdiv_divide_size_t(tile_index_ij_kl.quotient, range_j);
	const struct fxdiv_divisor_size_t tile_range_l = context->tile_range_l;
	const struct fxdiv_result_size_t tile_index_k_l = fxdiv_divide_size_t(tile_index_ij_kl.remainder, tile_range_l);
	const size_t max_tile_k = context->tile_k;
	const size_t max_tile_l = context->tile_l;
	const size_t index_i = tile_index_i_j.quotient;
	const size_t index_j = tile_index_i_j.remainder;
	const size_t index_k = tile_index_k_l.quotient * max_tile_k;
	const size_t index_l = tile_index_k_l.remainder * max_tile_l;
	const size_t tile_k = min(max_tile_k, context->range_k - index_k);
	const size_t tile_l = min(max_tile_l, context->range_l - index_l);
	context->task(context->argument, index_i, index_j, index_k, index_l, tile_k, tile_l);
}

void pthreadpool_parallelize_4d_tile_2d(
	pthreadpool_t threadpool,
	pthreadpool_task_4d_tile_2d_t task,
	void* argument,
	size_t range_i,
	size_t range_j,
	size_t range_k,
	size_t range_l,
	size_t tile_k,
	size_t tile_l,
	uint32_t flags)
{
	if (threadpool == NULL || threadpool->threads_count <= 1 || ((range_i | range_j) <= 1 && range_k <= tile_k && range_l <= tile_l)) {
		/* No thread pool used: execute task sequentially on the calling thread */
		struct fpu_state saved_fpu_state = { 0 };
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			saved_fpu_state = get_fpu_state();
			disable_fpu_denormals();
		}
		for (size_t i = 0; i < range_i; i++) {
			for (size_t j = 0; j < range_j; j++) {
				for (size_t k = 0; k < range_k; k += tile_k) {
					for (size_t l = 0; l < range_l; l += tile_l) {
						task(argument, i, j, k, l,
							min(range_k - k, tile_k), min(range_l - l, tile_l));
					}
				}
			}
		}
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			set_fpu_state(saved_fpu_state);
		}
	} else {
		/* Execute in parallel on the thread pool using linearized index */
		const size_t tile_range_k = divide_round_up(range_k, tile_k);
		const size_t tile_range_l = divide_round_up(range_l, tile_l);
		struct compute_4d_tile_2d_context context = {
			.task = task,
			.argument = argument,
			.tile_range_kl = fxdiv_init_size_t(tile_range_k * tile_range_l),
			.range_j = fxdiv_init_size_t(range_j),
			.tile_range_l = fxdiv_init_size_t(tile_range_l),
			.range_k = range_k,
			.range_l = range_l,
			.tile_k = tile_k,
			.tile_l = tile_l
		};
		pthreadpool_parallelize_1d(threadpool,
			(pthreadpool_task_1d_t) compute_4d_tile_2d, &context,
			range_i * range_j * tile_range_k * tile_range_l, flags);
	}
}

struct compute_5d_tile_2d_context {
	pthreadpool_task_5d_tile_2d_t task;
	void* argument;
	struct fxdiv_divisor_size_t tile_range_lm;
	struct fxdiv_divisor_size_t range_k;
	struct fxdiv_divisor_size_t tile_range_m;
	struct fxdiv_divisor_size_t range_j;
	size_t range_l;
	size_t range_m;
	size_t tile_l;
	size_t tile_m;
};

static void compute_5d_tile_2d(const struct compute_5d_tile_2d_context* context, size_t linear_index) {
	const struct fxdiv_divisor_size_t tile_range_lm = context->tile_range_lm;
	const struct fxdiv_result_size_t tile_index_ijk_lm = fxdiv_divide_size_t(linear_index, tile_range_lm);
	const struct fxdiv_divisor_size_t range_k = context->range_k;
	const struct fxdiv_result_size_t tile_index_ij_k = fxdiv_divide_size_t(tile_index_ijk_lm.quotient, range_k);
	const struct fxdiv_divisor_size_t tile_range_m = context->tile_range_m;
	const struct fxdiv_result_size_t tile_index_l_m = fxdiv_divide_size_t(tile_index_ijk_lm.remainder, tile_range_m);
	const struct fxdiv_divisor_size_t range_j = context->range_j;
	const struct fxdiv_result_size_t tile_index_i_j = fxdiv_divide_size_t(tile_index_ij_k.quotient, range_j);

	const size_t max_tile_l = context->tile_l;
	const size_t max_tile_m = context->tile_m;
	const size_t index_i = tile_index_i_j.quotient;
	const size_t index_j = tile_index_i_j.remainder;
	const size_t index_k = tile_index_ij_k.remainder;
	const size_t index_l = tile_index_l_m.quotient * max_tile_l;
	const size_t index_m = tile_index_l_m.remainder * max_tile_m;
	const size_t tile_l = min(max_tile_l, context->range_l - index_l);
	const size_t tile_m = min(max_tile_m, context->range_m - index_m);
	context->task(context->argument, index_i, index_j, index_k, index_l, index_m, tile_l, tile_m);
}

void pthreadpool_parallelize_5d_tile_2d(
	pthreadpool_t threadpool,
	pthreadpool_task_5d_tile_2d_t task,
	void* argument,
	size_t range_i,
	size_t range_j,
	size_t range_k,
	size_t range_l,
	size_t range_m,
	size_t tile_l,
	size_t tile_m,
	uint32_t flags)
{
	if (threadpool == NULL || threadpool->threads_count <= 1 || ((range_i | range_j | range_k) <= 1 && range_l <= tile_l && range_m <= tile_m)) {
		/* No thread pool used: execute task sequentially on the calling thread */
		struct fpu_state saved_fpu_state = { 0 };
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			saved_fpu_state = get_fpu_state();
			disable_fpu_denormals();
		}
		for (size_t i = 0; i < range_i; i++) {
			for (size_t j = 0; j < range_j; j++) {
				for (size_t k = 0; k < range_k; k++) {
					for (size_t l = 0; l < range_l; l += tile_l) {
						for (size_t m = 0; m < range_m; m += tile_m) {
							task(argument, i, j, k, l, m,
								min(range_l - l, tile_l), min(range_m - m, tile_m));
						}
					}
				}
			}
		}
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			set_fpu_state(saved_fpu_state);
		}
	} else {
		/* Execute in parallel on the thread pool using linearized index */
		const size_t tile_range_l = divide_round_up(range_l, tile_l);
		const size_t tile_range_m = divide_round_up(range_m, tile_m);
		struct compute_5d_tile_2d_context context = {
			.task = task,
			.argument = argument,
			.tile_range_lm = fxdiv_init_size_t(tile_range_l * tile_range_m),
			.range_k = fxdiv_init_size_t(range_k),
			.tile_range_m = fxdiv_init_size_t(tile_range_m),
			.range_j = fxdiv_init_size_t(range_j),
			.range_l = range_l,
			.range_m = range_m,
			.tile_l = tile_l,
			.tile_m = tile_m,
		};
		pthreadpool_parallelize_1d(threadpool,
			(pthreadpool_task_1d_t) compute_5d_tile_2d, &context,
			range_i * range_j * range_k * tile_range_l * tile_range_m, flags);
	}
}

struct compute_6d_tile_2d_context {
	pthreadpool_task_6d_tile_2d_t task;
	void* argument;
	struct fxdiv_divisor_size_t tile_range_lmn;
	struct fxdiv_divisor_size_t range_k;
	struct fxdiv_divisor_size_t tile_range_n;
	struct fxdiv_divisor_size_t range_j;
	struct fxdiv_divisor_size_t tile_range_m;
	size_t range_m;
	size_t range_n;
	size_t tile_m;
	size_t tile_n;
};

static void compute_6d_tile_2d(const struct compute_6d_tile_2d_context* context, size_t linear_index) {
	const struct fxdiv_divisor_size_t tile_range_lmn = context->tile_range_lmn;
	const struct fxdiv_result_size_t tile_index_ijk_lmn = fxdiv_divide_size_t(linear_index, tile_range_lmn);
	const struct fxdiv_divisor_size_t range_k = context->range_k;
	const struct fxdiv_result_size_t tile_index_ij_k = fxdiv_divide_size_t(tile_index_ijk_lmn.quotient, range_k);
	const struct fxdiv_divisor_size_t tile_range_n = context->tile_range_n;
	const struct fxdiv_result_size_t tile_index_lm_n = fxdiv_divide_size_t(tile_index_ijk_lmn.remainder, tile_range_n);
	const struct fxdiv_divisor_size_t range_j = context->range_j;
	const struct fxdiv_result_size_t tile_index_i_j = fxdiv_divide_size_t(tile_index_ij_k.quotient, range_j);
	const struct fxdiv_divisor_size_t tile_range_m = context->tile_range_m;
	const struct fxdiv_result_size_t tile_index_l_m = fxdiv_divide_size_t(tile_index_lm_n.quotient, tile_range_m);

	const size_t max_tile_m = context->tile_m;
	const size_t max_tile_n = context->tile_n;
	const size_t index_i = tile_index_i_j.quotient;
	const size_t index_j = tile_index_i_j.remainder;
	const size_t index_k = tile_index_ij_k.remainder;
	const size_t index_l = tile_index_l_m.quotient;
	const size_t index_m = tile_index_l_m.remainder * max_tile_m;
	const size_t index_n = tile_index_lm_n.remainder * max_tile_n;
	const size_t tile_m = min(max_tile_m, context->range_m - index_m);
	const size_t tile_n = min(max_tile_n, context->range_n - index_n);
	context->task(context->argument, index_i, index_j, index_k, index_l, index_m, index_n, tile_m, tile_n);
}

void pthreadpool_parallelize_6d_tile_2d(
	pthreadpool_t threadpool,
	pthreadpool_task_6d_tile_2d_t task,
	void* argument,
	size_t range_i,
	size_t range_j,
	size_t range_k,
	size_t range_l,
	size_t range_m,
	size_t range_n,
	size_t tile_m,
	size_t tile_n,
	uint32_t flags)
{
	if (threadpool == NULL || threadpool->threads_count <= 1 || ((range_i | range_j | range_k | range_l) <= 1 && range_m <= tile_m && range_n <= tile_n)) {
		/* No thread pool used: execute task sequentially on the calling thread */
		struct fpu_state saved_fpu_state = { 0 };
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			saved_fpu_state = get_fpu_state();
			disable_fpu_denormals();
		}
		for (size_t i = 0; i < range_i; i++) {
			for (size_t j = 0; j < range_j; j++) {
				for (size_t k = 0; k < range_k; k++) {
					for (size_t l = 0; l < range_l; l++) {
						for (size_t m = 0; m < range_m; m += tile_m) {
							for (size_t n = 0; n < range_n; n += tile_n) {
								task(argument, i, j, k, l, m, n,
									min(range_m - m, tile_m), min(range_n - n, tile_n));
							}
						}
					}
				}
			}
		}
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			set_fpu_state(saved_fpu_state);
		}
	} else {
		/* Execute in parallel on the thread pool using linearized index */
		const size_t tile_range_m = divide_round_up(range_m, tile_m);
		const size_t tile_range_n = divide_round_up(range_n, tile_n);
		struct compute_6d_tile_2d_context context = {
			.task = task,
			.argument = argument,
			.tile_range_lmn = fxdiv_init_size_t(range_l * tile_range_m * tile_range_n),
			.range_k = fxdiv_init_size_t(range_k),
			.tile_range_n = fxdiv_init_size_t(tile_range_n),
			.range_j = fxdiv_init_size_t(range_j),
			.tile_range_m = fxdiv_init_size_t(tile_range_m),
			.range_m = range_m,
			.range_n = range_n,
			.tile_m = tile_m,
			.tile_n = tile_n,
		};
		pthreadpool_parallelize_1d(threadpool,
			(pthreadpool_task_1d_t) compute_6d_tile_2d, &context,
			range_i * range_j * range_k * range_l * tile_range_m * tile_range_n, flags);
	}
}

void pthreadpool_destroy(struct pthreadpool* threadpool) {
	if (threadpool != NULL) {
		const size_t threads_count = threadpool->threads_count;
		if (threads_count > 1) {
			#if PTHREADPOOL_USE_FUTEX
				pthreadpool_store_relaxed_size_t(&threadpool->active_threads, threads_count - 1 /* caller thread */);
				pthreadpool_store_relaxed_uint32_t(&threadpool->has_active_threads, 1);

				/*
				 * Store the command with release semantics to guarantee that if a worker thread observes
				 * the new command value, it also observes the updated active_threads/has_active_threads values.
				 */
				pthreadpool_store_release_uint32_t(&threadpool->command, threadpool_command_shutdown);

				/* Wake up worker threads */
				futex_wake_all(&threadpool->command);
			#else
				/* Lock the command variable to ensure that threads don't shutdown until both command and active_threads are updated */
				pthread_mutex_lock(&threadpool->command_mutex);

				pthreadpool_store_relaxed_size_t(&threadpool->active_threads, threads_count - 1 /* caller thread */);

				/*
				 * Store the command with release semantics to guarantee that if a worker thread observes
				 * the new command value, it also observes the updated active_threads value.
				 *
				 * Note: the release fence inside pthread_mutex_unlock is insufficient,
				 * because the workers might be waiting in a spin-loop rather than the conditional variable.
				 */
				pthreadpool_store_release_uint32_t(&threadpool->command, threadpool_command_shutdown);

				/* Wake up worker threads */
				pthread_cond_broadcast(&threadpool->command_condvar);

				/* Commit the state changes and let workers start processing */
				pthread_mutex_unlock(&threadpool->command_mutex);
			#endif

			/* Wait until all threads return */
			for (size_t thread = 1; thread < threads_count; thread++) {
				pthread_join(threadpool->threads[thread].thread_object, NULL);
			}

			/* Release resources */
			pthread_mutex_destroy(&threadpool->execution_mutex);
			#if !PTHREADPOOL_USE_FUTEX
				pthread_mutex_destroy(&threadpool->completion_mutex);
				pthread_cond_destroy(&threadpool->completion_condvar);
				pthread_mutex_destroy(&threadpool->command_mutex);
				pthread_cond_destroy(&threadpool->command_condvar);
			#endif
		}
		#ifdef _WIN32
			_aligned_free(threadpool);
		#else
			free(threadpool);
		#endif
	}
}
