#include <gtest/gtest.h>

#include <pthreadpool.h>

const size_t itemsCount1D = 1024;

TEST(SetupAndShutdown, Basic) {
	pthreadpool* threadpool = pthreadpool_create(0);
	EXPECT_TRUE(threadpool != nullptr);
	pthreadpool_destroy(threadpool);
}

static void computeNothing1D(void*, size_t) {
}

TEST(Compute1D, Basic) {
	pthreadpool* threadpool = pthreadpool_create(0);
	EXPECT_TRUE(threadpool != nullptr);
	pthreadpool_compute_1d(threadpool, computeNothing1D, NULL, itemsCount1D);
	pthreadpool_destroy(threadpool);
}

static void checkRange1D(void*, size_t itemId) {
	EXPECT_LT(itemId, itemsCount1D);
}

TEST(Compute1D, ValidRange) {
	pthreadpool* threadpool = pthreadpool_create(0);
	EXPECT_TRUE(threadpool != nullptr);
	pthreadpool_compute_1d(threadpool, checkRange1D, NULL, itemsCount1D);
	pthreadpool_destroy(threadpool);
}

static void setTrue1D(bool indicators[], size_t itemId) {
	indicators[itemId] = true;
}

TEST(Compute1D, AllItemsProcessed) {
	bool processed[itemsCount1D];
	memset(processed, 0, sizeof(processed));

	pthreadpool* threadpool = pthreadpool_create(0);
	EXPECT_TRUE(threadpool != nullptr);
	pthreadpool_compute_1d(threadpool, reinterpret_cast<pthreadpool_function_1d_t>(setTrue1D), processed, itemsCount1D);
	for (size_t itemId = 0; itemId < itemsCount1D; itemId++) {
		EXPECT_TRUE(processed[itemId]) << "Item " << itemId << " not processed";
	}
	pthreadpool_destroy(threadpool);
}

static void increment1D(int counters[], size_t itemId) {
	counters[itemId] += 1;
}

TEST(Compute1D, EachItemProcessedOnce) {
	int processedCount[itemsCount1D];
	memset(processedCount, 0, sizeof(processedCount));

	pthreadpool* threadpool = pthreadpool_create(0);
	EXPECT_TRUE(threadpool != nullptr);
	pthreadpool_compute_1d(threadpool, reinterpret_cast<pthreadpool_function_1d_t>(increment1D), processedCount, itemsCount1D);
	for (size_t itemId = 0; itemId < itemsCount1D; itemId++) {
		EXPECT_EQ(1, processedCount[itemId]) << "Item " << itemId << " processed " << processedCount[itemId] << " times";
	}
	pthreadpool_destroy(threadpool);
}

TEST(Compute1D, EachItemProcessedMultipleTimes) {
	int processedCount[itemsCount1D];
	memset(processedCount, 0, sizeof(processedCount));
	const size_t iterations = 100;

	pthreadpool* threadpool = pthreadpool_create(0);
	EXPECT_TRUE(threadpool != nullptr);

	for (size_t iteration = 0; iteration < iterations; iteration++) {
		pthreadpool_compute_1d(threadpool, reinterpret_cast<pthreadpool_function_1d_t>(increment1D), processedCount, itemsCount1D);
	}
	for (size_t itemId = 0; itemId < itemsCount1D; itemId++) {
		EXPECT_EQ(iterations, processedCount[itemId]) << "Item " << itemId << " processed " << processedCount[itemId] << " times";
	}
	pthreadpool_destroy(threadpool);
}

static void workImbalance1D(volatile size_t* computedItems, size_t itemId) {
	__sync_fetch_and_add(computedItems, 1);
	if (itemId == 0) {
		/* Wait until all items are computed */
		while (*computedItems != itemsCount1D) {
			__sync_synchronize();
		}
	}
}

TEST(Compute1D, WorkStealing) {
	volatile size_t computedItems = 0;

	pthreadpool* threadpool = pthreadpool_create(0);
	EXPECT_TRUE(threadpool != nullptr);

	pthreadpool_compute_1d(threadpool, reinterpret_cast<pthreadpool_function_1d_t>(workImbalance1D), reinterpret_cast<void*>(const_cast<size_t*>(&computedItems)), itemsCount1D);
	EXPECT_EQ(computedItems, itemsCount1D);

	pthreadpool_destroy(threadpool);
}

int main(int argc, char* argv[]) {
	setenv("TERM", "xterm-256color", 0);
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
