#include "core/thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

}  // namespace

TEST(ThreadPool, RejectsZeroThreads) {
  EXPECT_THROW(search::ThreadPool pool(0), std::invalid_argument);
}

TEST(ThreadPool, ExecutesEverySubmittedTask) {
  search::ThreadPool pool(4);
  std::atomic<int> executed{0};

  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(pool.Submit([&executed] { executed.fetch_add(1); }));
  }
  pool.Shutdown();

  EXPECT_EQ(1000, executed.load());
}

TEST(ThreadPool, ExecutesEveryTaskWithManyThreads) {
  search::ThreadPool pool(8);
  std::atomic<int> executed{0};

  for (int i = 0; i < 5000; ++i) {
    ASSERT_TRUE(pool.Submit([&executed] { executed.fetch_add(1); }));
  }
  pool.Shutdown();

  EXPECT_EQ(5000, executed.load());
}

TEST(ThreadPool, PreservesOrderWithSingleThread) {
  search::ThreadPool pool(1);
  std::vector<int> order;

  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(pool.Submit([&order, i] { order.push_back(i); }));
  }
  pool.Shutdown();

  ASSERT_EQ(100U, order.size());
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(i, order[static_cast<std::size_t>(i)]);
  }
}

TEST(ThreadPool, ShutdownWaitsForRunningTask) {
  search::ThreadPool pool(2);
  std::atomic<bool> finished{false};

  ASSERT_TRUE(pool.Submit([&finished] {
    std::this_thread::sleep_for(150ms);
    finished.store(true);
  }));

  const auto started = std::chrono::steady_clock::now();
  pool.Shutdown();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_TRUE(finished.load());
  EXPECT_GE(elapsed, 100ms);
}

TEST(ThreadPool, KeepsRunningAfterTaskThrows) {
  search::ThreadPool pool(1);
  std::atomic<int> executed{0};

  ASSERT_TRUE(pool.Submit([] { throw std::runtime_error("boom"); }));
  ASSERT_TRUE(pool.Submit([&executed] { executed.fetch_add(1); }));
  ASSERT_TRUE(pool.Submit([&executed] { executed.fetch_add(1); }));
  pool.Shutdown();

  EXPECT_EQ(2, executed.load());
}

TEST(ThreadPool, RejectsTaskAfterShutdown) {
  search::ThreadPool pool(2);
  pool.Shutdown();

  EXPECT_FALSE(pool.Submit([] {}));
}

TEST(ThreadPool, RejectsEmptyTask) {
  search::ThreadPool pool(2);

  EXPECT_FALSE(pool.Submit(std::function<void()>{}));
}

TEST(ThreadPool, DestructorWaitsForQueuedTasks) {
  std::atomic<int> executed{0};
  {
    search::ThreadPool pool(2);
    for (int i = 0; i < 200; ++i) {
      ASSERT_TRUE(pool.Submit([&executed] { executed.fetch_add(1); }));
    }
  }

  EXPECT_EQ(200, executed.load());
}

TEST(ThreadPool, ReportsThreadCount) {
  const search::ThreadPool pool(3);

  EXPECT_EQ(3U, pool.ThreadCount());
}
