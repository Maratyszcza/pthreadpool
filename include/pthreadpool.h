#ifndef PTHREADPOOL_H_
#define PTHREADPOOL_H_

#include <stddef.h>
#include <stdint.h>

typedef struct pthreadpool* pthreadpool_t;

typedef void (*pthreadpool_task_1d_t)(void*, size_t);
typedef void (*pthreadpool_task_1d_tile_1d_t)(void*, size_t, size_t);
typedef void (*pthreadpool_task_2d_t)(void*, size_t, size_t);
typedef void (*pthreadpool_task_2d_tile_1d_t)(void*, size_t, size_t, size_t);
typedef void (*pthreadpool_task_2d_tile_2d_t)(void*, size_t, size_t, size_t, size_t);
typedef void (*pthreadpool_task_3d_tile_2d_t)(void*, size_t, size_t, size_t, size_t, size_t);
typedef void (*pthreadpool_task_4d_tile_2d_t)(void*, size_t, size_t, size_t, size_t, size_t, size_t);
typedef void (*pthreadpool_task_5d_tile_2d_t)(void*, size_t, size_t, size_t, size_t, size_t, size_t, size_t);
typedef void (*pthreadpool_task_6d_tile_2d_t)(void*, size_t, size_t, size_t, size_t, size_t, size_t, size_t, size_t);


#define PTHREADPOOL_FLAG_DISABLE_DENORMALS 0x00000001

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Creates a thread pool with the specified number of threads.
 *
 * @param[in]  threads_count  The number of threads in the thread pool.
 *    A value of 0 has special interpretation: it creates a thread for each
 *    processor core available in the system.
 *
 * @returns  A pointer to an opaque thread pool object.
 *    On error the function returns NULL and sets errno accordingly.
 */
pthreadpool_t pthreadpool_create(size_t threads_count);

/**
 * Queries the number of threads in a thread pool.
 *
 * @param[in]  threadpool  The thread pool to query.
 *
 * @returns  The number of threads in the thread pool.
 */
size_t pthreadpool_get_threads_count(pthreadpool_t threadpool);

/**
 * Processes items in parallel using threads from a thread pool.
 *
 * When the call returns, all items have been processed and the thread pool is
 * ready for a new task.
 *
 * @note If multiple threads call this function with the same thread pool, the
 *    calls are serialized.
 *
 * @param[in]  threadpool  The thread pool to use for parallelisation.
 * @param[in]  function    The function to call for each item.
 * @param[in]  argument    The first argument passed to the @a function.
 * @param[in]  items       The number of items to process. The @a function
 *    will be called once for each item.
 */
void pthreadpool_parallelize_1d(
	pthreadpool_t threadpool,
	pthreadpool_task_1d_t function,
	void* argument,
	size_t range,
	uint32_t flags);

void pthreadpool_parallelize_1d_tile_1d(
	pthreadpool_t threadpool,
	pthreadpool_task_1d_tile_1d_t function,
	void* argument,
	size_t range,
	size_t tile,
	uint32_t flags);

void pthreadpool_parallelize_2d(
	pthreadpool_t threadpool,
	pthreadpool_task_2d_t function,
	void* argument,
	size_t range_i,
	size_t range_j,
	uint32_t flags);

void pthreadpool_parallelize_2d_tile_1d(
	pthreadpool_t threadpool,
	pthreadpool_task_2d_tile_1d_t function,
	void* argument,
	size_t range_i,
	size_t range_j,
	size_t tile_j,
	uint32_t flags);

void pthreadpool_parallelize_2d_tile_2d(
	pthreadpool_t threadpool,
	pthreadpool_task_2d_tile_2d_t function,
	void* argument,
	size_t range_i,
	size_t range_j,
	size_t tile_i,
	size_t tile_j,
	uint32_t flags);

void pthreadpool_parallelize_3d_tile_2d(
	pthreadpool_t threadpool,
	pthreadpool_task_3d_tile_2d_t function,
	void* argument,
	size_t range_i,
	size_t range_j,
	size_t range_k,
	size_t tile_j,
	size_t tile_k,
	uint32_t flags);

void pthreadpool_parallelize_4d_tile_2d(
	pthreadpool_t threadpool,
	pthreadpool_task_4d_tile_2d_t function,
	void* argument,
	size_t range_i,
	size_t range_j,
	size_t range_k,
	size_t range_l,
	size_t tile_k,
	size_t tile_l,
	uint32_t flags);

void pthreadpool_parallelize_5d_tile_2d(
	pthreadpool_t threadpool,
	pthreadpool_task_5d_tile_2d_t function,
	void* argument,
	size_t range_i,
	size_t range_j,
	size_t range_k,
	size_t range_l,
	size_t range_m,
	size_t tile_l,
	size_t tile_m,
	uint32_t flags);

void pthreadpool_parallelize_6d_tile_2d(
	pthreadpool_t threadpool,
	pthreadpool_task_6d_tile_2d_t function,
	void* argument,
	size_t range_i,
	size_t range_j,
	size_t range_k,
	size_t range_l,
	size_t range_m,
	size_t range_n,
	size_t tile_m,
	size_t tile_n,
	uint32_t flags);

/**
 * Terminates threads in the thread pool and releases associated resources.
 *
 * @warning  Accessing the thread pool after a call to this function constitutes
 *    undefined behaviour and may cause data corruption.
 *
 * @param[in,out]  threadpool  The thread pool to destroy.
 */
void pthreadpool_destroy(pthreadpool_t threadpool);


#ifndef PTHREADPOOL_NO_DEPRECATED_API

/* Legacy API for compatibility with pre-existing users (e.g. NNPACK) */
#if defined(__GNUC__)
	#define PTHREADPOOL_DEPRECATED __attribute__((__deprecated__))
#else
	#define PTHREADPOOL_DEPRECATED
#endif

typedef void (*pthreadpool_function_1d_t)(void*, size_t) PTHREADPOOL_DEPRECATED;
typedef void (*pthreadpool_function_1d_tiled_t)(void*, size_t, size_t) PTHREADPOOL_DEPRECATED;
typedef void (*pthreadpool_function_2d_t)(void*, size_t, size_t) PTHREADPOOL_DEPRECATED;
typedef void (*pthreadpool_function_2d_tiled_t)(void*, size_t, size_t, size_t, size_t) PTHREADPOOL_DEPRECATED;
typedef void (*pthreadpool_function_3d_tiled_t)(void*, size_t, size_t, size_t, size_t, size_t, size_t) PTHREADPOOL_DEPRECATED;
typedef void (*pthreadpool_function_4d_tiled_t)(void*, size_t, size_t, size_t, size_t, size_t, size_t, size_t, size_t) PTHREADPOOL_DEPRECATED;

void pthreadpool_compute_1d(
	pthreadpool_t threadpool,
	pthreadpool_function_1d_t function,
	void* argument,
	size_t range) PTHREADPOOL_DEPRECATED;

void pthreadpool_compute_1d_tiled(
	pthreadpool_t threadpool,
	pthreadpool_function_1d_tiled_t function,
	void* argument,
	size_t range,
	size_t tile) PTHREADPOOL_DEPRECATED;

void pthreadpool_compute_2d(
	pthreadpool_t threadpool,
	pthreadpool_function_2d_t function,
	void* argument,
	size_t range_i,
	size_t range_j) PTHREADPOOL_DEPRECATED;

void pthreadpool_compute_2d_tiled(
	pthreadpool_t threadpool,
	pthreadpool_function_2d_tiled_t function,
	void* argument,
	size_t range_i,
	size_t range_j,
	size_t tile_i,
	size_t tile_j) PTHREADPOOL_DEPRECATED;

void pthreadpool_compute_3d_tiled(
	pthreadpool_t threadpool,
	pthreadpool_function_3d_tiled_t function,
	void* argument,
	size_t range_i,
	size_t range_j,
	size_t range_k,
	size_t tile_i,
	size_t tile_j,
	size_t tile_k) PTHREADPOOL_DEPRECATED;

void pthreadpool_compute_4d_tiled(
	pthreadpool_t threadpool,
	pthreadpool_function_4d_tiled_t function,
	void* argument,
	size_t range_i,
	size_t range_j,
	size_t range_k,
	size_t range_l,
	size_t tile_i,
	size_t tile_j,
	size_t tile_k,
	size_t tile_l) PTHREADPOOL_DEPRECATED;

#endif /* PTHREADPOOL_NO_DEPRECATED_API */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PTHREADPOOL_H_ */
