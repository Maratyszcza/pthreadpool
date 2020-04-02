/* Standard C headers */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if PTHREADPOOL_USE_CPUINFO
	#include <cpuinfo.h>
#endif

/* Dependencies */
#include <fxdiv.h>

/* Library header */
#include <pthreadpool.h>

/* Internal headers */
#include "threadpool-utils.h"
#include "threadpool-atomics.h"
#include "threadpool-object.h"


size_t pthreadpool_get_threads_count(struct pthreadpool* threadpool) {
	if (threadpool == NULL) {
		return 1;
	}

	return threadpool->threads_count;
}

static void thread_parallelize_1d(struct pthreadpool* threadpool, struct thread_info* thread) {
	assert(threadpool != NULL);
	assert(thread != NULL);

	const pthreadpool_task_1d_t task = (pthreadpool_task_1d_t) pthreadpool_load_relaxed_void_p(&threadpool->task);
	void *const argument = pthreadpool_load_relaxed_void_p(&threadpool->argument);
	/* Process thread's own range of items */
	size_t range_start = pthreadpool_load_relaxed_size_t(&thread->range_start);
	while (pthreadpool_try_decrement_relaxed_size_t(&thread->range_length)) {
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
		while (pthreadpool_try_decrement_relaxed_size_t(&other_thread->range_length)) {
			const size_t item_id = pthreadpool_fetch_sub_relaxed_size_t(&other_thread->range_end, 1) - 1;
			task(argument, item_id);
		}
	}

	/* Make changes by this thread visible to other threads */
	pthreadpool_fence_release();
}

static void thread_parallelize_1d_with_uarch(struct pthreadpool* threadpool, struct thread_info* thread) {
	assert(threadpool != NULL);
	assert(thread != NULL);

	const pthreadpool_task_1d_with_id_t task = (pthreadpool_task_1d_with_id_t) pthreadpool_load_relaxed_void_p(&threadpool->task);
	void *const argument = pthreadpool_load_relaxed_void_p(&threadpool->argument);

	const uint32_t default_uarch_index = threadpool->params.parallelize_1d_with_uarch.default_uarch_index;
	uint32_t uarch_index = default_uarch_index;
	#if PTHREADPOOL_USE_CPUINFO
		uarch_index = cpuinfo_get_current_uarch_index();
		if (uarch_index > threadpool->params.parallelize_1d_with_uarch.max_uarch_index) {
			uarch_index = default_uarch_index;
		}
	#endif

	/* Process thread's own range of items */
	size_t range_start = pthreadpool_load_relaxed_size_t(&thread->range_start);
	while (pthreadpool_try_decrement_relaxed_size_t(&thread->range_length)) {
		task(argument, uarch_index, range_start++);
	}

	/* There still may be other threads with work */
	const size_t thread_number = thread->thread_number;
	const size_t threads_count = threadpool->threads_count;
	for (size_t tid = modulo_decrement(thread_number, threads_count);
		tid != thread_number;
		tid = modulo_decrement(tid, threads_count))
	{
		struct thread_info* other_thread = &threadpool->threads[tid];
		while (pthreadpool_try_decrement_relaxed_size_t(&other_thread->range_length)) {
			const size_t item_id = pthreadpool_fetch_sub_relaxed_size_t(&other_thread->range_end, 1) - 1;
			task(argument, uarch_index, item_id);
		}
	}

	/* Make changes by this thread visible to other threads */
	pthreadpool_fence_release();
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
		pthreadpool_parallelize(
			threadpool, &thread_parallelize_1d, NULL, 0,
			(void*) task, argument, range, flags);
	}
}

void pthreadpool_parallelize_1d_with_uarch(
	pthreadpool_t threadpool,
	pthreadpool_task_1d_with_id_t task,
	void* argument,
	uint32_t default_uarch_index,
	uint32_t max_uarch_index,
	size_t range,
	uint32_t flags)
{
	if (threadpool == NULL || threadpool->threads_count <= 1 || range <= 1) {
		/* No thread pool used: execute task sequentially on the calling thread */

		uint32_t uarch_index = default_uarch_index;
		#if PTHREADPOOL_USE_CPUINFO
			uarch_index = cpuinfo_get_current_uarch_index();
			if (uarch_index > max_uarch_index) {
				uarch_index = default_uarch_index;
			}
		#endif

		struct fpu_state saved_fpu_state = { 0 };
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			saved_fpu_state = get_fpu_state();
			disable_fpu_denormals();
		}
		for (size_t i = 0; i < range; i++) {
			task(argument, uarch_index, i);
		}
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			set_fpu_state(saved_fpu_state);
		}
	} else {
		const struct pthreadpool_1d_with_uarch_params params = {
			.default_uarch_index = default_uarch_index,
			.max_uarch_index = max_uarch_index,
		};
		pthreadpool_parallelize(
			threadpool, &thread_parallelize_1d_with_uarch, &params, sizeof(params),
			task, argument, range, flags);
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
		pthreadpool_parallelize(
			threadpool, &thread_parallelize_1d, NULL, 0,
			(void*) compute_1d_tile_1d, &context, tile_range, flags);
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
		pthreadpool_parallelize(
			threadpool, &thread_parallelize_1d, NULL, 0,
			(void*) compute_2d, &context, range_i * range_j, flags);
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
		pthreadpool_parallelize(
			threadpool, &thread_parallelize_1d, NULL, 0,
			(void*) compute_2d_tile_1d, &context, range_i * tile_range_j, flags);
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
		pthreadpool_parallelize(
			threadpool, &thread_parallelize_1d, NULL, 0,
			(void*) compute_2d_tile_2d, &context, tile_range_i * tile_range_j, flags);
	}
}

struct compute_2d_tile_2d_with_uarch_context {
	pthreadpool_task_2d_tile_2d_with_id_t task;
	void* argument;
	struct fxdiv_divisor_size_t tile_range_j;
	size_t range_i;
	size_t range_j;
	size_t tile_i;
	size_t tile_j;
};

static void compute_2d_tile_2d_with_uarch(const struct compute_2d_tile_2d_with_uarch_context* context, uint32_t uarch_index, size_t linear_index) {
	const struct fxdiv_divisor_size_t tile_range_j = context->tile_range_j;
	const struct fxdiv_result_size_t tile_index = fxdiv_divide_size_t(linear_index, tile_range_j);
	const size_t max_tile_i = context->tile_i;
	const size_t max_tile_j = context->tile_j;
	const size_t index_i = tile_index.quotient * max_tile_i;
	const size_t index_j = tile_index.remainder * max_tile_j;
	const size_t tile_i = min(max_tile_i, context->range_i - index_i);
	const size_t tile_j = min(max_tile_j, context->range_j - index_j);
	context->task(context->argument, uarch_index, index_i, index_j, tile_i, tile_j);
}

void pthreadpool_parallelize_2d_tile_2d_with_uarch(
	pthreadpool_t threadpool,
	pthreadpool_task_2d_tile_2d_with_id_t task,
	void* argument,
	uint32_t default_uarch_index,
	uint32_t max_uarch_index,
	size_t range_i,
	size_t range_j,
	size_t tile_i,
	size_t tile_j,
	uint32_t flags)
{
	if (threadpool == NULL || threadpool->threads_count <= 1 || (range_i <= tile_i && range_j <= tile_j)) {
		/* No thread pool used: execute task sequentially on the calling thread */

		uint32_t uarch_index = default_uarch_index;
		#if PTHREADPOOL_USE_CPUINFO
			uarch_index = cpuinfo_get_current_uarch_index();
			if (uarch_index > max_uarch_index) {
				uarch_index = default_uarch_index;
			}
		#endif

		struct fpu_state saved_fpu_state = { 0 };
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			saved_fpu_state = get_fpu_state();
			disable_fpu_denormals();
		}
		for (size_t i = 0; i < range_i; i += tile_i) {
			for (size_t j = 0; j < range_j; j += tile_j) {
				task(argument, uarch_index, i, j, min(range_i - i, tile_i), min(range_j - j, tile_j));
			}
		}
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			set_fpu_state(saved_fpu_state);
		}
	} else {
		/* Execute in parallel on the thread pool using linearized index */
		const size_t tile_range_i = divide_round_up(range_i, tile_i);
		const size_t tile_range_j = divide_round_up(range_j, tile_j);
		const struct pthreadpool_1d_with_uarch_params params = {
			.default_uarch_index = default_uarch_index,
			.max_uarch_index = max_uarch_index,
		};
		struct compute_2d_tile_2d_with_uarch_context context = {
			.task = task,
			.argument = argument,
			.tile_range_j = fxdiv_init_size_t(tile_range_j),
			.range_i = range_i,
			.range_j = range_j,
			.tile_i = tile_i,
			.tile_j = tile_j
		};
		pthreadpool_parallelize(
			threadpool, &thread_parallelize_1d_with_uarch, &params, sizeof(params),
			(void*) compute_2d_tile_2d_with_uarch, &context, tile_range_i * tile_range_j, flags);
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
		pthreadpool_parallelize(
			threadpool, &thread_parallelize_1d, NULL, 0,
			(void*) compute_3d_tile_2d, &context, range_i * tile_range_j * tile_range_k, flags);
	}
}

struct compute_3d_tile_2d_with_uarch_context {
	pthreadpool_task_3d_tile_2d_with_id_t task;
	void* argument;
	struct fxdiv_divisor_size_t tile_range_j;
	struct fxdiv_divisor_size_t tile_range_k;
	size_t range_j;
	size_t range_k;
	size_t tile_j;
	size_t tile_k;
};

static void compute_3d_tile_2d_with_uarch(const struct compute_3d_tile_2d_with_uarch_context* context, uint32_t uarch_index, size_t linear_index) {
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
	context->task(context->argument, uarch_index, index_i, index_j, index_k, tile_j, tile_k);
}

void pthreadpool_parallelize_3d_tile_2d_with_uarch(
	pthreadpool_t threadpool,
	pthreadpool_task_3d_tile_2d_with_id_t task,
	void* argument,
	uint32_t default_uarch_index,
	uint32_t max_uarch_index,
	size_t range_i,
	size_t range_j,
	size_t range_k,
	size_t tile_j,
	size_t tile_k,
	uint32_t flags)
{
	if (threadpool == NULL || threadpool->threads_count <= 1 || (range_i <= 1 && range_j <= tile_j && range_k <= tile_k)) {
		/* No thread pool used: execute task sequentially on the calling thread */

		uint32_t uarch_index = default_uarch_index;
		#if PTHREADPOOL_USE_CPUINFO
			uarch_index = cpuinfo_get_current_uarch_index();
			if (uarch_index > max_uarch_index) {
				uarch_index = default_uarch_index;
			}
		#endif

		struct fpu_state saved_fpu_state = { 0 };
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			saved_fpu_state = get_fpu_state();
			disable_fpu_denormals();
		}
		for (size_t i = 0; i < range_i; i++) {
			for (size_t j = 0; j < range_j; j += tile_j) {
				for (size_t k = 0; k < range_k; k += tile_k) {
					task(argument, uarch_index, i, j, k, min(range_j - j, tile_j), min(range_k - k, tile_k));
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
		const struct pthreadpool_1d_with_uarch_params params = {
			.default_uarch_index = default_uarch_index,
			.max_uarch_index = max_uarch_index,
		};
		struct compute_3d_tile_2d_with_uarch_context context = {
			.task = task,
			.argument = argument,
			.tile_range_j = fxdiv_init_size_t(tile_range_j),
			.tile_range_k = fxdiv_init_size_t(tile_range_k),
			.range_j = range_j,
			.range_k = range_k,
			.tile_j = tile_j,
			.tile_k = tile_k
		};
		pthreadpool_parallelize(
			threadpool, &thread_parallelize_1d_with_uarch, &params, sizeof(params),
			(void*) compute_3d_tile_2d_with_uarch, &context, range_i * tile_range_j * tile_range_k, flags);
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
		pthreadpool_parallelize(
			threadpool, &thread_parallelize_1d, NULL, 0,
			(void*) compute_4d_tile_2d, &context, range_i * range_j * tile_range_k * tile_range_l, flags);
	}
}

struct compute_4d_tile_2d_with_uarch_context {
	pthreadpool_task_4d_tile_2d_with_id_t task;
	void* argument;
	struct fxdiv_divisor_size_t tile_range_kl;
	struct fxdiv_divisor_size_t range_j;
	struct fxdiv_divisor_size_t tile_range_l;
	size_t range_k;
	size_t range_l;
	size_t tile_k;
	size_t tile_l;
};

static void compute_4d_tile_2d_with_uarch(const struct compute_4d_tile_2d_with_uarch_context* context, uint32_t uarch_index, size_t linear_index) {
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
	context->task(context->argument, uarch_index, index_i, index_j, index_k, index_l, tile_k, tile_l);
}

void pthreadpool_parallelize_4d_tile_2d_with_uarch(
	pthreadpool_t threadpool,
	pthreadpool_task_4d_tile_2d_with_id_t task,
	void* argument,
	uint32_t default_uarch_index,
	uint32_t max_uarch_index,
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

		uint32_t uarch_index = default_uarch_index;
		#if PTHREADPOOL_USE_CPUINFO
			uarch_index = cpuinfo_get_current_uarch_index();
			if (uarch_index > max_uarch_index) {
				uarch_index = default_uarch_index;
			}
		#endif

		struct fpu_state saved_fpu_state = { 0 };
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			saved_fpu_state = get_fpu_state();
			disable_fpu_denormals();
		}
		for (size_t i = 0; i < range_i; i++) {
			for (size_t j = 0; j < range_j; j++) {
				for (size_t k = 0; k < range_k; k += tile_k) {
					for (size_t l = 0; l < range_l; l += tile_l) {
						task(argument, uarch_index, i, j, k, l,
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
		const struct pthreadpool_1d_with_uarch_params params = {
			.default_uarch_index = default_uarch_index,
			.max_uarch_index = max_uarch_index,
		};
		struct compute_4d_tile_2d_with_uarch_context context = {
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
		pthreadpool_parallelize(
			threadpool, &thread_parallelize_1d_with_uarch, &params, sizeof(params),
			(void*) compute_4d_tile_2d_with_uarch, &context, range_i * range_j * tile_range_k * tile_range_l, flags);
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
		pthreadpool_parallelize(
			threadpool, &thread_parallelize_1d, NULL, 0,
			(void*) compute_5d_tile_2d, &context, range_i * range_j * range_k * tile_range_l * tile_range_m, flags);
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
		pthreadpool_parallelize(
			threadpool, &thread_parallelize_1d, NULL, 0,
			(void*) compute_6d_tile_2d, &context, range_i * range_j * range_k * range_l * tile_range_m * tile_range_n, flags);
	}
}
