#include <benchmark/benchmark.h>

#include <unistd.h>

#include <pthreadpool.h>


static void SetNumberOfThreads(benchmark::internal::Benchmark* benchmark) {
	const int maxThreads = sysconf(_SC_NPROCESSORS_ONLN);
	for (int t = 0; t <= maxThreads; t++) {
		benchmark->Arg(t);
	}
}


static void compute_1d(void* context, size_t x) {
}

static void pthreadpool_compute_1d(benchmark::State& state) {
	const uint32_t threads = static_cast<uint32_t>(state.range(0));
	pthreadpool_t threadpool = threads == 0 ? NULL : pthreadpool_create(threads);
	while (state.KeepRunning()) {
		pthreadpool_compute_1d(threadpool, compute_1d, NULL, threads);
	}
	pthreadpool_destroy(threadpool);
}
BENCHMARK(pthreadpool_compute_1d)->UseRealTime()->Apply(SetNumberOfThreads);


static void compute_1d_tiled(void* context, size_t x0, size_t xn) {
}

static void pthreadpool_compute_1d_tiled(benchmark::State& state) {
	const uint32_t threads = static_cast<uint32_t>(state.range(0));
	pthreadpool_t threadpool = threads == 0 ? NULL : pthreadpool_create(threads);
	while (state.KeepRunning()) {
		pthreadpool_compute_1d_tiled(threadpool, compute_1d_tiled, NULL, threads, 1);
	}
	pthreadpool_destroy(threadpool);
}
BENCHMARK(pthreadpool_compute_1d_tiled)->UseRealTime()->Apply(SetNumberOfThreads);


static void compute_2d(void* context, size_t x, size_t y) {
}

static void pthreadpool_compute_2d(benchmark::State& state) {
	const uint32_t threads = static_cast<uint32_t>(state.range(0));
	pthreadpool_t threadpool = threads == 0 ? NULL : pthreadpool_create(threads);
	while (state.KeepRunning()) {
		pthreadpool_compute_2d(threadpool, compute_2d, NULL, 1, threads);
	}
	pthreadpool_destroy(threadpool);
}
BENCHMARK(pthreadpool_compute_2d)->UseRealTime()->Apply(SetNumberOfThreads);


static void compute_2d_tiled(void* context, size_t x0, size_t y0, size_t xn, size_t yn) {
}

static void pthreadpool_compute_2d_tiled(benchmark::State& state) {
	const uint32_t threads = static_cast<uint32_t>(state.range(0));
	pthreadpool_t threadpool = threads == 0 ? NULL : pthreadpool_create(threads);
	while (state.KeepRunning()) {
		pthreadpool_compute_2d_tiled(threadpool, compute_2d_tiled, NULL, 1, threads, 1, 1);
	}
	pthreadpool_destroy(threadpool);
}
BENCHMARK(pthreadpool_compute_2d_tiled)->UseRealTime()->Apply(SetNumberOfThreads);


BENCHMARK_MAIN();
