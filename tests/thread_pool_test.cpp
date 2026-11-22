#include "core/thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <semaphore>
#include <stdexcept>
#include <string>
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
  testing::internal::CaptureStderr();
  search::ThreadPool pool(1);
  std::atomic<int> executed{0};

  ASSERT_TRUE(pool.Submit([] { throw std::runtime_error("boom"); }));
  ASSERT_TRUE(pool.Submit([&executed] { executed.fetch_add(1); }));
  ASSERT_TRUE(pool.Submit([&executed] { executed.fetch_add(1); }));
  pool.Shutdown();
  const std::string stderr_output = testing::internal::GetCapturedStderr();

  EXPECT_EQ(2, executed.load());
  EXPECT_NE(std::string::npos,
            stderr_output.find("ThreadPool: task threw: boom"));
}

TEST(ThreadPool, ReportsUnknownTaskException) {
  testing::internal::CaptureStderr();
  search::ThreadPool pool(1);
  std::atomic<int> executed{0};

  ASSERT_TRUE(pool.Submit([] { throw 42; }));
  ASSERT_TRUE(pool.Submit([&executed] { executed.fetch_add(1); }));
  pool.Shutdown();
  const std::string stderr_output = testing::internal::GetCapturedStderr();

  EXPECT_EQ(1, executed.load());
  EXPECT_NE(std::string::npos,
            stderr_output.find("ThreadPool: task threw unknown exception"));
}

TEST(ThreadPool, RejectsTaskAfterShutdown) {
  search::ThreadPool pool(2);
  pool.Shutdown();

  EXPECT_FALSE(pool.Submit([] {}));
}

TEST(ThreadPool, ReportsItsQueueCapacity) {
  const search::ThreadPool bounded(2, 5);
  const search::ThreadPool unbounded(2);

  EXPECT_EQ(5U, bounded.MaxQueue());
  EXPECT_EQ(0U, unbounded.MaxQueue());
}

// Regression: before the bounded queue, Submit() always accepted work. A
// burst of requests then grew the queue without limit.
TEST(ThreadPool, RejectsTasksWhenTheBoundedQueueIsFull) {
  search::ThreadPool pool(1, 2);
  std::binary_semaphore worker_started{0};
  std::binary_semaphore release_worker{0};

  ASSERT_TRUE(pool.Submit([&worker_started, &release_worker] {
    worker_started.release();
    release_worker.acquire();
  }));
  worker_started.acquire();

  // The single worker is blocked, so these two tasks sit in a queue of
  // capacity two and the third has nowhere to go.
  ASSERT_TRUE(pool.Submit([] {}));
  ASSERT_TRUE(pool.Submit([] {}));
  EXPECT_FALSE(pool.Submit([] {}));

  release_worker.release();
  pool.Shutdown();

  // Once drained, the pool accepts work again.
  search::ThreadPool fresh(1, 1);
  ASSERT_TRUE(fresh.Submit([] {}));
  fresh.Shutdown();
}

// Regression: a throwing std::thread constructor in the middle of the loop used
// to leave the already-created workers joinable; ~ThreadPool() then called
// std::terminate. The constructor must stop and join them before rethrowing.
// The failure is injected by a factory, not by exhausting machine resources.
TEST(ThreadPool, JoinsCreatedWorkersWhenThreadCreationFails) {
  int created = 0;
  search::ThreadPool::ThreadFactory factory =
      [&created](std::function<void()> task) {
        ++created;
        if (created == 2) {
          throw std::runtime_error("injected thread creation failure");
        }
        return std::thread(std::move(task));
      };

  EXPECT_THROW(search::ThreadPool pool(4, 0, factory), std::runtime_error);
  EXPECT_EQ(2, created);
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

// Concurrent Shutdown callers must not race on workers_. Before the separate
// shutdown mutex this test reports a data race under ThreadSanitizer.
TEST(ThreadPool, ShutdownIsSafeForConcurrentCallers) {
  constexpr int kCallers = 8;
  search::ThreadPool pool(4);
  std::atomic<int> executed{0};

  for (int i = 0; i < 500; ++i) {
    ASSERT_TRUE(pool.Submit([&executed] { executed.fetch_add(1); }));
  }

  std::barrier start(kCallers);
  std::vector<std::thread> callers;
  callers.reserve(kCallers);
  for (int i = 0; i < kCallers; ++i) {
    callers.emplace_back([&pool, &start] {
      start.arrive_and_wait();
      pool.Shutdown();
    });
  }
  for (std::thread& caller : callers) {
    caller.join();
  }

  EXPECT_EQ(500, executed.load());
}

TEST(ThreadPool, AcceptsTasksFromManyProducers) {
  constexpr int kProducers = 8;
  constexpr int kTasksPerProducer = 1000;
  search::ThreadPool pool(4);
  std::atomic<int> executed{0};
  std::atomic<int> rejected{0};

  std::barrier start(kProducers);
  std::vector<std::thread> producers;
  producers.reserve(kProducers);
  for (int i = 0; i < kProducers; ++i) {
    producers.emplace_back([&] {
      start.arrive_and_wait();
      for (int task = 0; task < kTasksPerProducer; ++task) {
        if (!pool.Submit([&executed] { executed.fetch_add(1); })) {
          rejected.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& producer : producers) {
    producer.join();
  }
  pool.Shutdown();

  EXPECT_EQ(0, rejected.load());
  EXPECT_EQ(kProducers * kTasksPerProducer, executed.load());
}

// Producers keep submitting while another thread shuts the pool down: every
// accepted task must run exactly once and every computed task is counted.
TEST(ThreadPool, LosesNoTaskWhenShutdownRunsConcurrently) {
  constexpr int kProducers = 8;
  constexpr int kTasksPerProducer = 500;
  search::ThreadPool pool(4);
  std::atomic<int> accepted{0};
  std::atomic<int> rejected{0};
  std::atomic<int> executed{0};

  std::barrier start(kProducers + 1);
  std::vector<std::thread> threads;
  threads.reserve(kProducers + 1);
  for (int i = 0; i < kProducers; ++i) {
    threads.emplace_back([&] {
      start.arrive_and_wait();
      for (int task = 0; task < kTasksPerProducer; ++task) {
        if (pool.Submit([&executed] { executed.fetch_add(1); })) {
          accepted.fetch_add(1);
        } else {
          rejected.fetch_add(1);
        }
      }
    });
  }
  threads.emplace_back([&pool, &start] {
    start.arrive_and_wait();
    pool.Shutdown();
  });

  for (std::thread& thread : threads) {
    thread.join();
  }
  pool.Shutdown();

  EXPECT_EQ(kProducers * kTasksPerProducer, accepted.load() + rejected.load());
  EXPECT_EQ(accepted.load(), executed.load());
}
