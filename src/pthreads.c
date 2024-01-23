/* Standard C headers */
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Configuration header */
#include "threadpool-common.h"

/* POSIX headers */
#include <pthread.h>
#include <unistd.h>

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

/* Windows-specific headers */
#ifdef _WIN32
	#include <sysinfoapi.h>
#endif

/* Dependencies */
#if PTHREADPOOL_USE_CPUINFO
	#include <cpuinfo.h>
#endif

/* Public library header */
#include <pthreadpool.h>

/* Internal library headers */
#include "threadpool-atomics.h"
#include "threadpool-object.h"
#include "threadpool-utils.h"


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

static void checkin_worker_thread(struct pthreadpool* threadpool) {
	#if PTHREADPOOL_USE_FUTEX
		if (pthreadpool_decrement_fetch_acquire_release_size_t(&threadpool->active_threads) == 0) {
			pthreadpool_store_release_uint32_t(&threadpool->has_active_threads, 0);
			futex_wake_all(&threadpool->has_active_threads);
		}
	#else
		pthread_mutex_lock(&threadpool->completion_mutex);
		if (pthreadpool_decrement_fetch_release_size_t(&threadpool->active_threads) == 0) {
			pthread_cond_signal(&threadpool->completion_condvar);
		}
		pthread_mutex_unlock(&threadpool->completion_mutex);
	#endif
}

static void wait_worker_threads(struct pthreadpool* threadpool) {
	/* Initial check */
	#if PTHREADPOOL_USE_FUTEX
		uint32_t has_active_threads = pthreadpool_load_acquire_uint32_t(&threadpool->has_active_threads);
		if (has_active_threads == 0) {
			return;
		}
	#else
		size_t active_threads = pthreadpool_load_acquire_size_t(&threadpool->active_threads);
		if (active_threads == 0) {
			return;
		}
	#endif

	/* Spin-wait */
	for (uint32_t i = PTHREADPOOL_SPIN_WAIT_ITERATIONS; i != 0; i--) {
		pthreadpool_yield();

		#if PTHREADPOOL_USE_FUTEX
			has_active_threads = pthreadpool_load_acquire_uint32_t(&threadpool->has_active_threads);
			if (has_active_threads == 0) {
				return;
			}
		#else
			active_threads = pthreadpool_load_acquire_size_t(&threadpool->active_threads);
			if (active_threads == 0) {
				return;
			}
		#endif
	}

	/* Fall-back to mutex/futex wait */
	#if PTHREADPOOL_USE_FUTEX
		while ((has_active_threads = pthreadpool_load_acquire_uint32_t(&threadpool->has_active_threads)) != 0) {
			futex_wait(&threadpool->has_active_threads, 1);
		}
	#else
		pthread_mutex_lock(&threadpool->completion_mutex);
		while (pthreadpool_load_acquire_size_t(&threadpool->active_threads) != 0) {
			pthread_cond_wait(&threadpool->completion_condvar, &threadpool->completion_mutex);
		};
		pthread_mutex_unlock(&threadpool->completion_mutex);
	#endif
}

static uint32_t wait_for_new_command(
	struct thread_info* thread,
	uint32_t last_command,
	uint32_t last_flags)
{
	uint32_t command = pthreadpool_load_acquire_uint32_t(&thread->command);
	if (command != last_command) {
		return command;
	}

	if ((last_flags & PTHREADPOOL_FLAG_YIELD_WORKERS) == 0) {
		/* Spin-wait loop */
		for (uint32_t i = PTHREADPOOL_SPIN_WAIT_ITERATIONS; i != 0; i--) {
			pthreadpool_yield();

			command = pthreadpool_load_acquire_uint32_t(&thread->command);
			if (command != last_command) {
				return command;
			}
		}
	}

	/* Spin-wait disabled or timed out, fall back to mutex/futex wait */
	#if PTHREADPOOL_USE_FUTEX
		do {
			futex_wait(&thread->command, last_command);
			command = pthreadpool_load_acquire_uint32_t(&thread->command);
		} while (command == last_command);
	#else
		/* Lock the command mutex */
		pthread_mutex_lock(&thread->command_mutex);
		/* Read the command */
		while ((command = pthreadpool_load_acquire_uint32_t(&thread->command)) == last_command) {
			/* Wait for new command */
			pthread_cond_wait(&thread->command_condvar, &thread->command_mutex);
		}
		/* Read a new command */
		pthread_mutex_unlock(&thread->command_mutex);
	#endif
	return command;
}

static void* thread_main(void* arg) {
	struct thread_info* thread = (struct thread_info*) arg;
	struct pthreadpool* threadpool = thread->threadpool;
	uint32_t last_command = threadpool_command_init;
	struct fpu_state saved_fpu_state = { 0 };
	uint32_t flags = 0;

	/* Check in */
	checkin_worker_thread(threadpool);

	/* Monitor new commands and act accordingly */
	for (;;) {
		uint32_t command = wait_for_new_command(thread, last_command, flags);
		pthreadpool_fence_acquire();

		flags = pthreadpool_load_relaxed_uint32_t(&threadpool->flags);

		/* Process command */
		switch (command & THREADPOOL_COMMAND_MASK) {
			case threadpool_command_parallelize:
			{
				const thread_function_t thread_function =
					(thread_function_t) pthreadpool_load_relaxed_void_p(&threadpool->thread_function);
				if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
					saved_fpu_state = get_fpu_state();
					disable_fpu_denormals();
				}

				thread_function(threadpool, thread);
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

struct pthreadpool* pthreadpool_create(size_t max_threads_count) {
	#if PTHREADPOOL_USE_CPUINFO
		if (!cpuinfo_initialize()) {
			return NULL;
		}
	#endif

	if (max_threads_count == 0) {
		#if PTHREADPOOL_USE_CPUINFO
			max_threads_count = cpuinfo_get_processors_count();
		#elif defined(_SC_NPROCESSORS_ONLN)
			max_threads_count = (size_t) sysconf(_SC_NPROCESSORS_ONLN);
			#if defined(__EMSCRIPTEN_PTHREADS__)
				/* Limit the number of threads to 8 to match link-time PTHREAD_POOL_SIZE option */
				if (max_threads_count >= 8) {
					max_threads_count = 8;
				}
			#endif
		#elif defined(_WIN32)
			SYSTEM_INFO system_info;
			ZeroMemory(&system_info, sizeof(system_info));
			GetSystemInfo(&system_info);
			max_threads_count = (size_t) system_info.dwNumberOfProcessors;
		#else
			#error "Platform-specific implementation of sysconf(_SC_NPROCESSORS_ONLN) required"
		#endif
	}

	struct pthreadpool* threadpool = pthreadpool_allocate(max_threads_count);
	if (threadpool == NULL) {
		return NULL;
	}
	threadpool->max_threads_count = fxdiv_init_size_t(max_threads_count);
	pthreadpool_store_relaxed_size_t(&threadpool->threads_count, max_threads_count);
	for (size_t tid = 0; tid < max_threads_count; tid++) {
		threadpool->threads[tid].thread_number = tid;
		threadpool->threads[tid].threadpool = threadpool;
		// Since command is per thread we are creating conditional variables per thread as well.
		// However, only children thread participate in wait/wakeup signalling.
#if PTHREADPOOL_USE_CONDVAR
		pthread_mutex_init(&(threadpool->threads[tid].command_mutex), NULL);
		pthread_cond_init(&(threadpool->threads[tid].command_condvar), NULL);
#endif
	}

	/* Thread pool with a single thread computes everything on the caller thread. */
	if (max_threads_count > 1) {
		pthread_mutex_init(&threadpool->execution_mutex, NULL);
		#if !PTHREADPOOL_USE_FUTEX
			pthread_mutex_init(&threadpool->completion_mutex, NULL);
			pthread_cond_init(&threadpool->completion_condvar, NULL);
		#endif

		#if PTHREADPOOL_USE_FUTEX
			pthreadpool_store_relaxed_uint32_t(&threadpool->has_active_threads, 1);
		#endif
		pthreadpool_store_relaxed_size_t(&threadpool->active_threads, max_threads_count - 1 /* caller thread */);

		/* Caller thread serves as worker #0. Thus, we create system threads starting with worker #1. */
		for (size_t tid = 1; tid < max_threads_count; tid++) {
			pthread_create(&threadpool->threads[tid].thread_object, NULL, &thread_main, &threadpool->threads[tid]);
		}

		/* Wait until all threads initialize */
		wait_worker_threads(threadpool);
	}
	return threadpool;
}

void pthreadpool_set_threads_count(struct pthreadpool* threadpool, size_t num_threads) {
	pthread_mutex_lock(&threadpool->execution_mutex);
	const struct fxdiv_divisor_size_t max_threads_count = threadpool->max_threads_count;
	const size_t num_threads_to_use = min(max_threads_count.value, num_threads);
	pthreadpool_store_release_size_t(&threadpool->threads_count, num_threads_to_use);
	pthread_mutex_unlock(&threadpool->execution_mutex);
}

PTHREADPOOL_INTERNAL void pthreadpool_parallelize(
	struct pthreadpool* threadpool,
	thread_function_t thread_function,
	const void* params,
	size_t params_size,
	void* task,
	void* context,
	size_t linear_range,
	uint32_t flags)
{
	assert(threadpool != NULL);
	assert(thread_function != NULL);
	assert(task != NULL);
	assert(linear_range > 1);

	/* Protect the global threadpool structures */
	pthread_mutex_lock(&threadpool->execution_mutex);

	const struct fxdiv_divisor_size_t threads_count =
		fxdiv_init_size_t(pthreadpool_load_relaxed_size_t(&threadpool->threads_count));
	#if !PTHREADPOOL_USE_FUTEX
		/* Lock the command variables to ensure that threads don't start processing before they observe complete command with all arguments */
		for (size_t tid = 1; tid < threads_count.value; tid++) {
			pthread_mutex_lock(&(threadpool->threads[tid].command_mutex));
		}
	#endif

	/* Setup global arguments */
	pthreadpool_store_relaxed_void_p(&threadpool->thread_function, (void*) thread_function);
	pthreadpool_store_relaxed_void_p(&threadpool->task, task);
	pthreadpool_store_relaxed_void_p(&threadpool->argument, context);
	pthreadpool_store_relaxed_uint32_t(&threadpool->flags, flags);

	/* Locking of completion_mutex not needed: readers are sleeping on command_condvar */
	pthreadpool_store_relaxed_size_t(&threadpool->active_threads, threads_count.value - 1 /* caller thread */);
	#if PTHREADPOOL_USE_FUTEX
		pthreadpool_store_relaxed_uint32_t(&threadpool->has_active_threads, 1);
	#endif

	if (params_size != 0) {
		memcpy(&threadpool->params, params, params_size);
		pthreadpool_fence_release();
	}

	/* Spread the work between threads */
	const struct fxdiv_result_size_t range_params = fxdiv_divide_size_t(linear_range, threads_count);
	size_t range_start = 0;
	for (size_t tid = 0; tid < threads_count.value; tid++) {
		struct thread_info* thread = &threadpool->threads[tid];
		const size_t range_length = range_params.quotient + (size_t) (tid < range_params.remainder);
		const size_t range_end = range_start + range_length;
		pthreadpool_store_relaxed_size_t(&thread->range_start, range_start);
		pthreadpool_store_relaxed_size_t(&thread->range_end, range_end);
		pthreadpool_store_relaxed_size_t(&thread->range_length, range_length);

		/* The next subrange starts where the previous ended */
		range_start = range_end;
	}

	for (size_t tid = 1; tid < threads_count.value; tid++) {
		/*
		 * Update the threadpool command.
		 * Imporantly, do it after initializing command parameters (range, task, argument, flags)
		 * ~(threadpool->command | THREADPOOL_COMMAND_MASK) flips the bits not in command mask
		 * to ensure the unmasked command is different then the last command, because worker threads
		 * monitor for change in the unmasked command.
		 */
		const uint32_t old_command = pthreadpool_load_relaxed_uint32_t(&(threadpool->threads[tid].command));
		const uint32_t new_command = ~(old_command | THREADPOOL_COMMAND_MASK) | threadpool_command_parallelize;

		/*
		 * Store the command with release semantics to guarantee that if a worker thread observes
		 * the new command value, it also observes the updated command parameters.
		 *
		 * Note: release semantics is necessary even with a conditional variable, because the workers might
		 * be waiting in a spin-loop rather than the conditional variable.
		 */
		pthreadpool_store_release_uint32_t(&(threadpool->threads[tid].command), new_command);
		#if PTHREADPOOL_USE_FUTEX
			/* Wake up the threads */
			futex_wake_all(&(threadpool->threads[tid].command));
		#else
			/* Unlock the command variables before waking up the threads for better performance */
			pthread_mutex_unlock(&(threadpool->threads[tid].command_mutex));

			/* Wake up the threads */
			pthread_cond_broadcast(&(threadpool->threads[tid].command_condvar));
		#endif
	}

	/* Save and modify FPU denormals control, if needed */
	struct fpu_state saved_fpu_state = { 0 };
	if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
		saved_fpu_state = get_fpu_state();
		disable_fpu_denormals();
	}

	/* Do computations as worker #0 */
	thread_function(threadpool, &threadpool->threads[0]);

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

void pthreadpool_destroy(struct pthreadpool* threadpool) {
	if (threadpool != NULL) {
		const struct fxdiv_divisor_size_t max_threads_count = threadpool->max_threads_count;
		if (max_threads_count.value > 1) {
			#if PTHREADPOOL_USE_FUTEX
				pthreadpool_store_relaxed_size_t(&threadpool->active_threads, max_threads_count.value - 1 /* caller thread */);
				pthreadpool_store_relaxed_uint32_t(&threadpool->has_active_threads, 1);

				/* Wake up worker threads */
				for (size_t tid = 1; tid < max_threads_count.value; tid++) {
					/*
					 * Store the command with release semantics to guarantee that if a worker thread observes
					 * the new command value, it also observes the updated active_threads/has_active_threads values.
					 */
					pthreadpool_store_release_uint32_t(&threadpool->threads[tid].command, threadpool_command_shutdown);

					futex_wake_all(&threadpool->threads[tid].command);
				}
			#else
				for (size_t tid = 1; tid < max_threads_count.value; tid++) {
					/* Lock the command variable to ensure that threads don't shutdown until both command and active_threads are updated */
					pthread_mutex_lock(&threadpool->threads[tid].command_mutex);
				}
				pthreadpool_store_relaxed_size_t(&threadpool->active_threads, max_threads_count.value - 1 /* caller thread */);

				for (size_t tid = 1; tid < max_threads_count.value; tid++) {
					/*
					 * Store the command with release semantics to guarantee that if a worker thread observes
					 * the new command value, it also observes the updated active_threads value.
					 *
					 * Note: the release fence inside pthread_mutex_unlock is insufficient,
					 * because the workers might be waiting in a spin-loop rather than the conditional variable.
					 */
					pthreadpool_store_release_uint32_t(&threadpool->threads[tid].command, threadpool_command_shutdown);

					/* Wake up worker threads */
					pthread_cond_broadcast(&threadpool->threads[tid].command_condvar);
				}

				for (size_t tid = 1; tid < max_threads_count.value; tid++) {
					/* Commit the state changes and let workers start processing */
					pthread_mutex_unlock(&threadpool->threads[tid].command_mutex);
				}
			#endif

			/* Wait until all threads return */
			for (size_t thread = 1; thread < max_threads_count.value; thread++) {
				pthread_join(threadpool->threads[thread].thread_object, NULL);
			}

			/* Release resources */
			pthread_mutex_destroy(&threadpool->execution_mutex);
			#if !PTHREADPOOL_USE_FUTEX
				pthread_mutex_destroy(&threadpool->completion_mutex);
				pthread_cond_destroy(&threadpool->completion_condvar);
				for (size_t tid = 0; tid < max_threads_count.value; tid++) {
					pthread_mutex_destroy(&threadpool->threads[tid].command_mutex);
					pthread_cond_destroy(&threadpool->threads[tid].command_condvar);
				}
			#endif
		}
		#if PTHREADPOOL_USE_CPUINFO
			cpuinfo_deinitialize();
		#endif
		pthreadpool_deallocate(threadpool);
	}
}
