#include <benchmark/benchmark.h>

#include <unistd.h>

#include <pthreadpool.h>

#ifdef _WIN32
#	include <sysinfoapi.h>
#endif

static void SetNumberOfThreads(benchmark::internal::Benchmark* benchmark) {
#ifdef _WIN32
	SYSTEM_INFO system_info;
	ZeroMemory(&system_info, sizeof(system_info));
	GetSystemInfo(&system_info);
	const int max_threads = (size_t) system_info.dwNumberOfProcessors;
#else
	const int max_threads = sysconf(_SC_NPROCESSORS_ONLN);
#endif
	for (int t = 1; t <= max_threads; t++) {
		benchmark->Arg(t);
	}
}


static void compute_1d(void*, size_t x) {
}

static void pthreadpool_parallelize_1d(benchmark::State& state) {
	const uint32_t threads = static_cast<uint32_t>(state.range(0));
	pthreadpool_t threadpool = pthreadpool_create(threads);
	while (state.KeepRunning()) {
		pthreadpool_parallelize_1d(
			threadpool,
			compute_1d,
			nullptr /* context */,
			threads,
			0 /* flags */);
	}
	pthreadpool_destroy(threadpool);
}
BENCHMARK(pthreadpool_parallelize_1d)->UseRealTime()->Apply(SetNumberOfThreads);


static void compute_1d_tile_1d(void*, size_t, size_t) {
}

static void pthreadpool_parallelize_1d_tile_1d(benchmark::State& state) {
	const uint32_t threads = static_cast<uint32_t>(state.range(0));
	pthreadpool_t threadpool = pthreadpool_create(threads);
	while (state.KeepRunning()) {
		pthreadpool_parallelize_1d_tile_1d(
			threadpool,
			compute_1d_tile_1d,
			nullptr /* context */,
			threads, 1,
			0 /* flags */);
	}
	pthreadpool_destroy(threadpool);
}
BENCHMARK(pthreadpool_parallelize_1d_tile_1d)->UseRealTime()->Apply(SetNumberOfThreads);


static void compute_2d(void*, size_t, size_t) {
}

static void pthreadpool_parallelize_2d(benchmark::State& state) {
	const uint32_t threads = static_cast<uint32_t>(state.range(0));
	pthreadpool_t threadpool = pthreadpool_create(threads);
	while (state.KeepRunning()) {
		pthreadpool_parallelize_2d(
			threadpool,
			compute_2d,
			nullptr /* context */,
			1, threads,
			0 /* flags */);
	}
	pthreadpool_destroy(threadpool);
}
BENCHMARK(pthreadpool_parallelize_2d)->UseRealTime()->Apply(SetNumberOfThreads);


static void compute_2d_tile_2d(void*, size_t, size_t, size_t, size_t) {
}

static void pthreadpool_parallelize_2d_tile_2d(benchmark::State& state) {
	const uint32_t threads = static_cast<uint32_t>(state.range(0));
	pthreadpool_t threadpool = pthreadpool_create(threads);
	while (state.KeepRunning()) {
		pthreadpool_parallelize_2d_tile_2d(
			threadpool,
			compute_2d_tile_2d,
			nullptr /* context */,
			1, threads,
			1, 1,
			0 /* flags */);
	}
	pthreadpool_destroy(threadpool);
}
BENCHMARK(pthreadpool_parallelize_2d_tile_2d)->UseRealTime()->Apply(SetNumberOfThreads);


BENCHMARK_MAIN();
