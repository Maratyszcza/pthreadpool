/* Standard C headers */
#include <stdint.h>
#include <stdbool.h>
#include <malloc.h>
#include <string.h>
#include <assert.h>

/* POSIX headers */
#include <pthread.h>
#include <unistd.h>

/* Library header */
#include <pthreadpool.h>

#define PTHREADPOOL_CACHELINE_SIZE 64
#define PTHREADPOOL_CACHELINE_ALIGNED __attribute__((__aligned__(PTHREADPOOL_CACHELINE_SIZE)))
#define PTHREADPOOL_STATIC_ASSERT(predicate, message) _Static_assert((predicate), message)

enum thread_state {
	thread_state_idle,
	thread_state_compute_1d,
	thread_state_shutdown,
};

struct PTHREADPOOL_CACHELINE_ALIGNED thread_info {
	/**
	 * Index of the first element in the work range.
	 * Before processing a new element the owning worker thread increments this value.
	 */
	volatile size_t range_start;
	/**
	 * Index of the element after the last element of the work range.
	 * Before processing a new element the stealing worker thread decrements this value.
	 */
	volatile size_t range_end;
	/**
	 * The number of elements in the work range.
	 * Due to race conditions range_length <= range_end - range_start.
	 * The owning worker thread must decrement this value before incrementing @a range_start.
	 * The stealing worker thread must decrement this value before decrementing @a range_end.
	 */
	volatile size_t range_length;
	/**
	 * The active state of the thread.
	 */
	volatile enum thread_state state;
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
	 * The number of threads that signalled completion of an operation.
	 */
	volatile size_t checkedin_threads;
	/**
	 * The function to call for each item.
	 */
	volatile pthreadpool_function_1d_t function;
	/**
	 * The first argument to the item processing function.
	 */
	void *volatile argument;
	/**
	 * Serializes concurrent calls to @a pthreadpool_compute_* from different threads.
	 */
	pthread_mutex_t execution_mutex;
	/**
	 * Guards access to the @a checkedin_threads variable.
	 */
	pthread_mutex_t barrier_mutex;
	/**
	 * Condition variable to wait until all threads check in.
	 */
	pthread_cond_t barrier_condvar;
	/**
	 * Guards access to the @a state variables.
	 */
	pthread_mutex_t state_mutex;
	/**
	 * Condition variable to wait for change of @a state variable.
	 */
	pthread_cond_t state_condvar;
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
	pthread_mutex_lock(&threadpool->barrier_mutex);
	const size_t checkedin_threads = threadpool->checkedin_threads + 1;
	threadpool->checkedin_threads = checkedin_threads;
	if (checkedin_threads == threadpool->threads_count) {
		pthread_cond_signal(&threadpool->barrier_condvar);
	}
	pthread_mutex_unlock(&threadpool->barrier_mutex);
}

static void wait_worker_threads(struct pthreadpool* threadpool) {
	if (threadpool->checkedin_threads != threadpool->threads_count) {
		pthread_mutex_lock(&threadpool->barrier_mutex);
		while (threadpool->checkedin_threads != threadpool->threads_count) {
			pthread_cond_wait(&threadpool->barrier_condvar, &threadpool->barrier_mutex);
		};
		pthread_mutex_unlock(&threadpool->barrier_mutex);
	}
}

static void wakeup_worker_threads(struct pthreadpool* threadpool) {
	pthread_mutex_lock(&threadpool->state_mutex);
	threadpool->checkedin_threads = 0; /* Locking of barrier_mutex not needed: readers are sleeping */
	pthread_cond_broadcast(&threadpool->state_condvar);
	pthread_mutex_unlock(&threadpool->state_mutex); /* Do wake up */
}

inline static bool atomic_decrement(volatile size_t* value) {
	size_t actual_value = *value;
	if (actual_value != 0) {
		size_t expected_value;
		do {
			expected_value = actual_value;
			const size_t new_value = actual_value - 1;
			actual_value = __sync_val_compare_and_swap(value, expected_value, new_value);
		} while ((actual_value != expected_value) && (actual_value != 0));
	}
	return actual_value != 0;
}

static void thread_compute_1d(struct pthreadpool* threadpool, struct thread_info* thread) {
	const pthreadpool_function_1d_t function = threadpool->function;
	void *const argument = threadpool->argument;
	/* Process thread's own range of items */
	size_t range_start = thread->range_start;
	while (atomic_decrement(&thread->range_length)) {
		function(argument, range_start++);
	}
	/* Done, now look for other threads' items to steal */
	const size_t thread_number = thread->thread_number;
	const size_t threads_count = threadpool->threads_count;
	for (size_t tid = (thread_number + 1) % threads_count; tid != thread_number; tid = (tid + 1) % threads_count) {
		struct thread_info* other_thread = &threadpool->threads[tid];
		if (other_thread->state != thread_state_idle) {
			while (atomic_decrement(&other_thread->range_length)) {
				const size_t item_id = __sync_sub_and_fetch(&other_thread->range_end, 1);
				function(argument, item_id);
			}
		}
	}
}

static void* thread_main(void* arg) {
	struct thread_info* thread = (struct thread_info*) arg;
	struct pthreadpool* threadpool = ((struct pthreadpool*) (thread - thread->thread_number)) - 1;

	/* Check in */
	checkin_worker_thread(threadpool);

	/* Monitor the state changes and act accordingly */
	for (;;) {
		/* Lock the state mutex */
		pthread_mutex_lock(&threadpool->state_mutex);
		/* Read the state */
		enum thread_state state;
		while ((state = thread->state) == thread_state_idle) {
			/* Wait for state change */
			pthread_cond_wait(&threadpool->state_condvar, &threadpool->state_mutex);
		}
		/* Read non-idle state */
		pthread_mutex_unlock(&threadpool->state_mutex);
		switch (state) {
			case thread_state_compute_1d:
				thread_compute_1d(threadpool, thread);
				break;
			case thread_state_shutdown:
				return NULL;
			case thread_state_idle:
				/* To inhibit compiler warning */
				break;
		}
		/* Notify the master thread that we finished processing */
		thread->state = thread_state_idle;
		checkin_worker_thread(threadpool);
	};
}

struct pthreadpool* pthreadpool_create(size_t threads_count) {
	if (threads_count == 0) {
		threads_count = (size_t) sysconf(_SC_NPROCESSORS_ONLN);
	}
	struct pthreadpool* threadpool = memalign(64, sizeof(struct pthreadpool) + threads_count * sizeof(struct thread_info));
	memset(threadpool, 0, sizeof(struct pthreadpool) + threads_count * sizeof(struct thread_info));
	threadpool->threads_count = threads_count;
	pthread_mutex_init(&threadpool->execution_mutex, NULL);
	pthread_mutex_init(&threadpool->barrier_mutex, NULL);
	pthread_cond_init(&threadpool->barrier_condvar, NULL);
	pthread_mutex_init(&threadpool->state_mutex, NULL);
	pthread_cond_init(&threadpool->state_condvar, NULL);

	for (size_t tid = 0; tid < threads_count; tid++) {
		threadpool->threads[tid].thread_number = tid;
		pthread_create(&threadpool->threads[tid].thread_object, NULL, &thread_main, &threadpool->threads[tid]);
	}

	/* Wait until all threads initialize */
	wait_worker_threads(threadpool);
	return threadpool;
}

uint32_t pthreadpool_get_threads_count(struct pthreadpool* threadpool) {
	return threadpool->threads_count;
}

static inline size_t multiply_divide(size_t a, size_t b, size_t d) {
	#if defined(__SIZEOF_SIZE_T__) && (__SIZEOF_SIZE_T__ == 4)
		return (size_t) (((uint64_t) a) * ((uint64_t) b)) / ((uint64_t) d);
	#elif defined(__SIZEOF_SIZE_T__) && (__SIZEOF_SIZE_T__ == 8)
		return (size_t) (((uint128_t) a) * ((uint128_t) b)) / ((uint128_t) d);
	#else
		#error "Unsupported platform"
	#endif
}

void pthreadpool_compute_1d(
	struct pthreadpool* threadpool,
	pthreadpool_function_1d_t function,
	void* argument,
	size_t items)
{
	/* Protect the global threadpool structures */
	pthread_mutex_lock(&threadpool->execution_mutex);

	/* Spread the work between threads */
	for (size_t tid = 0; tid < threadpool->threads_count; tid++) {
		struct thread_info* thread = &threadpool->threads[tid];
		thread->range_start = multiply_divide(items, tid, threadpool->threads_count);
		thread->range_end = multiply_divide(items, tid + 1, threadpool->threads_count);
		thread->range_length = thread->range_end - thread->range_start;
		thread->state = thread_state_compute_1d;
	}

	/* Setup global arguments */
	threadpool->function = function;
	threadpool->argument = argument;

	/* Wake up the threads */
	wakeup_worker_threads(threadpool);

	/* Wait until the threads finish computation */
	wait_worker_threads(threadpool);

	/* Unprotect the global threadpool structures */
	pthread_mutex_unlock(&threadpool->execution_mutex);
}

void pthreadpool_destroy(struct pthreadpool* threadpool) {
	/* Update threads' states */
	for (size_t tid = 0; tid < threadpool->threads_count; tid++) {
		threadpool->threads[tid].state = thread_state_shutdown;
	}

	/* Wake up the threads */
	wakeup_worker_threads(threadpool);

	/* Wait until all threads return */
	for (size_t tid = 0; tid < threadpool->threads_count; tid++) {
		pthread_join(threadpool->threads[tid].thread_object, NULL);
	}

	/* Release resources */
	pthread_mutex_destroy(&threadpool->execution_mutex);
	pthread_mutex_destroy(&threadpool->barrier_mutex);
	pthread_cond_destroy(&threadpool->barrier_condvar);
	pthread_mutex_destroy(&threadpool->state_mutex);
	pthread_cond_destroy(&threadpool->state_condvar);
	free(threadpool);
}
