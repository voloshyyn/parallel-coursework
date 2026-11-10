#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace search {

// Fixed-size pool of worker threads that execute queued tasks.
//
// The pool owns its threads, so tasks must not outlive it: the destructor
// waits for every queued task to finish. Exceptions thrown by a task are
// swallowed to keep the remaining tasks running.
class ThreadPool {
 public:
  // Throws std::invalid_argument when thread_count is zero.
  explicit ThreadPool(std::size_t thread_count);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // Enqueues a task and wakes one worker. Returns false when the pool has
  // already started shutting down, in which case the task is not executed.
  bool Submit(std::function<void()> task);

  // Stops accepting tasks, waits until the queue is drained and every running
  // task is finished, then joins the workers.
  //
  // Thread-safe: concurrent callers all return only after the pool has fully
  // stopped, and repeated calls afterwards do nothing. Calling Shutdown from a
  // task executed by this same pool is NOT supported: the worker would wait for
  // itself to finish and deadlock.
  void Shutdown();

  // Number of worker threads created at construction.
  std::size_t ThreadCount() const noexcept { return thread_count_; }

 private:
  void WorkerLoop();

  const std::size_t thread_count_;
  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  mutable std::mutex mutex_;
  // Serialises shutdown so only one caller joins workers_ at a time. Held only
  // while joining; mutex_ must not be held there.
  std::mutex shutdown_mutex_;
  std::condition_variable has_task_;
  bool stopping_ = false;
};

}  // namespace search
