#include <benchmark/benchmark.h>

#include <pthreadpool.h>


static void compute_1d(void*, size_t) {
}

static void pthreadpool_parallelize_1d(benchmark::State& state) {
	pthreadpool_t threadpool = pthreadpool_create(0);
	const size_t threads = pthreadpool_get_threads_count(threadpool);
	const size_t items = static_cast<size_t>(state.range(0));
	while (state.KeepRunning()) {
		pthreadpool_parallelize_1d(
			threadpool,
			compute_1d,
			nullptr /* context */,
			items * threads,
			0 /* flags */);
	}
	pthreadpool_destroy(threadpool);

	/* Do not normalize by thread */
	state.SetItemsProcessed(int64_t(state.iterations()) * items);
}
BENCHMARK(pthreadpool_parallelize_1d)->UseRealTime()->RangeMultiplier(10)->Range(10, 1000000);


static void compute_1d_tile_1d(void*, size_t, size_t) {
}

static void pthreadpool_parallelize_1d_tile_1d(benchmark::State& state) {
	pthreadpool_t threadpool = pthreadpool_create(0);
	const size_t threads = pthreadpool_get_threads_count(threadpool);
	const size_t items = static_cast<size_t>(state.range(0));
	while (state.KeepRunning()) {
		pthreadpool_parallelize_1d_tile_1d(
			threadpool,
			compute_1d_tile_1d,
			nullptr /* context */,
			items * threads, 1,
			0 /* flags */);
	}
	pthreadpool_destroy(threadpool);

	/* Do not normalize by thread */
	state.SetItemsProcessed(int64_t(state.iterations()) * items);
}
BENCHMARK(pthreadpool_parallelize_1d_tile_1d)->UseRealTime()->RangeMultiplier(10)->Range(10, 1000000);


static void compute_2d(void* context, size_t x, size_t y) {
}

static void pthreadpool_parallelize_2d(benchmark::State& state) {
	pthreadpool_t threadpool = pthreadpool_create(0);
	const size_t threads = pthreadpool_get_threads_count(threadpool);
	const size_t items = static_cast<size_t>(state.range(0));
	while (state.KeepRunning()) {
		pthreadpool_parallelize_2d(
			threadpool,
			compute_2d,
			nullptr /* context */,
			threads, items,
			0 /* flags */);
	}
	pthreadpool_destroy(threadpool);

	/* Do not normalize by thread */
	state.SetItemsProcessed(int64_t(state.iterations()) * items);
}
BENCHMARK(pthreadpool_parallelize_2d)->UseRealTime()->RangeMultiplier(10)->Range(10, 1000000);


static void compute_2d_tiled(void* context, size_t x0, size_t y0, size_t xn, size_t yn) {
}

static void pthreadpool_parallelize_2d_tile_2d(benchmark::State& state) {
	pthreadpool_t threadpool = pthreadpool_create(0);
	const size_t threads = pthreadpool_get_threads_count(threadpool);
	const size_t items = static_cast<size_t>(state.range(0));
	while (state.KeepRunning()) {
		pthreadpool_parallelize_2d_tile_2d(
			threadpool,
			compute_2d_tiled,
			nullptr /* context */,
			threads, items,
			1, 1,
			0 /* flags */);
	}
	pthreadpool_destroy(threadpool);

	/* Do not normalize by thread */
	state.SetItemsProcessed(int64_t(state.iterations()) * items);
}
BENCHMARK(pthreadpool_parallelize_2d_tile_2d)->UseRealTime()->RangeMultiplier(10)->Range(10, 1000000);


BENCHMARK_MAIN();
