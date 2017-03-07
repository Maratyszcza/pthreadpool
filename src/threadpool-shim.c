/* Standard C headers */
#include <stddef.h>

/* Library header */
#include <pthreadpool.h>

static inline size_t min(size_t a, size_t b) {
	return a < b ? a : b;
}

struct pthreadpool* pthreadpool_create(size_t threads_count) {
	return NULL;
}

size_t pthreadpool_get_threads_count(struct pthreadpool* threadpool) {
	return 1;
}

void pthreadpool_compute_1d(
	struct pthreadpool* threadpool,
	pthreadpool_function_1d_t function,
	void* argument,
	size_t range)
{
	for (size_t i = 0; i < range; i++) {
		function(argument, i);
	}
}

void pthreadpool_compute_1d_tiled(
	pthreadpool_t threadpool,
	pthreadpool_function_1d_tiled_t function,
	void* argument,
	size_t range,
	size_t tile)
{
	for (size_t i = 0; i < range; i += tile) {
		function(argument, i, min(range - i, tile));
	}
}

void pthreadpool_compute_2d(
	struct pthreadpool* threadpool,
	pthreadpool_function_2d_t function,
	void* argument,
	size_t range_i,
	size_t range_j)
{
	for (size_t i = 0; i < range_i; i++) {
		for (size_t j = 0; j < range_j; j++) {
			function(argument, i, j);
		}
	}
}

void pthreadpool_compute_2d_tiled(
	pthreadpool_t threadpool,
	pthreadpool_function_2d_tiled_t function,
	void* argument,
	size_t range_i,
	size_t range_j,
	size_t tile_i,
	size_t tile_j)
{
	for (size_t i = 0; i < range_i; i += tile_i) {
		for (size_t j = 0; j < range_j; j += tile_j) {
			function(argument, i, j, min(range_i - i, tile_i), min(range_j - j, tile_j));
		}
	}
}

void pthreadpool_destroy(struct pthreadpool* threadpool) {
}
