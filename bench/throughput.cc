#include <benchmark/benchmark.h>

#include <unistd.h>

#include <pthreadpool.h>


static void compute_1d(void* context, size_t x) {
}

static void pthreadpool_compute_1d(benchmark::State& state) {
	pthreadpool_t threadpool = pthreadpool_create(0);
	const size_t threads = pthreadpool_get_threads_count(threadpool);
	const size_t items = static_cast<size_t>(state.range(0));
	while (state.KeepRunning()) {
		pthreadpool_compute_1d(threadpool, compute_1d, NULL, items * threads);
	}
	pthreadpool_destroy(threadpool);

	/* Do not normalize by thread */
	state.SetItemsProcessed(int64_t(state.iterations()) * items);
}
BENCHMARK(pthreadpool_compute_1d)->UseRealTime()->RangeMultiplier(10)->Range(10, 1000000);


static void compute_1d_tiled(void* context, size_t x0, size_t xn) {
}

static void pthreadpool_compute_1d_tiled(benchmark::State& state) {
	pthreadpool_t threadpool = pthreadpool_create(0);
	const size_t threads = pthreadpool_get_threads_count(threadpool);
	const size_t items = static_cast<size_t>(state.range(0));
	while (state.KeepRunning()) {
		pthreadpool_compute_1d_tiled(threadpool, compute_1d_tiled, NULL, items * threads, 1);
	}
	pthreadpool_destroy(threadpool);

	/* Do not normalize by thread */
	state.SetItemsProcessed(int64_t(state.iterations()) * items);
}
BENCHMARK(pthreadpool_compute_1d_tiled)->UseRealTime()->RangeMultiplier(10)->Range(10, 1000000);


static void compute_2d(void* context, size_t x, size_t y) {
}

static void pthreadpool_compute_2d(benchmark::State& state) {
	pthreadpool_t threadpool = pthreadpool_create(0);
	const size_t threads = pthreadpool_get_threads_count(threadpool);
	const size_t items = static_cast<size_t>(state.range(0));
	while (state.KeepRunning()) {
		pthreadpool_compute_2d(threadpool, compute_2d, NULL, threads, items);
	}
	pthreadpool_destroy(threadpool);

	/* Do not normalize by thread */
	state.SetItemsProcessed(int64_t(state.iterations()) * items);
}
BENCHMARK(pthreadpool_compute_2d)->UseRealTime()->RangeMultiplier(10)->Range(10, 1000000);


static void compute_2d_tiled(void* context, size_t x0, size_t y0, size_t xn, size_t yn) {
}

static void pthreadpool_compute_2d_tiled(benchmark::State& state) {
	pthreadpool_t threadpool = pthreadpool_create(0);
	const size_t threads = pthreadpool_get_threads_count(threadpool);
	const size_t items = static_cast<size_t>(state.range(0));
	while (state.KeepRunning()) {
		pthreadpool_compute_2d_tiled(threadpool, compute_2d_tiled, NULL, threads, items, 1, 1);
	}
	pthreadpool_destroy(threadpool);

	/* Do not normalize by thread */
	state.SetItemsProcessed(int64_t(state.iterations()) * items);
}
BENCHMARK(pthreadpool_compute_2d_tiled)->UseRealTime()->RangeMultiplier(10)->Range(10, 1000000);


BENCHMARK_MAIN();
