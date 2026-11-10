#include "core/thread_pool.h"

#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace search {
namespace {

// Serialises diagnostics so lines from different workers never interleave.
void LogTaskFailure(const std::string& message) {
  static std::mutex log_mutex;
  std::lock_guard<std::mutex> lock(log_mutex);
  const std::string line = message + "\n";
  std::cerr.write(line.data(), static_cast<std::streamsize>(line.size()));
}

}  // namespace

ThreadPool::ThreadPool(std::size_t thread_count) : thread_count_(thread_count) {
  if (thread_count_ == 0) {
    throw std::invalid_argument("thread pool requires at least one thread");
  }

  workers_.reserve(thread_count_);
  for (std::size_t i = 0; i < thread_count_; ++i) {
    workers_.emplace_back(&ThreadPool::WorkerLoop, this);
  }
}

ThreadPool::~ThreadPool() { Shutdown(); }

bool ThreadPool::Submit(std::function<void()> task) {
  if (!task) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return false;
    }
    tasks_.push(std::move(task));
  }
  has_task_.notify_one();
  return true;
}

void ThreadPool::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  has_task_.notify_all();

  // Workers need mutex_ to drain the queue, so the join happens under a
  // separate mutex. Concurrent callers queue here and then see workers_ empty.
  std::lock_guard<std::mutex> shutdown_lock(shutdown_mutex_);
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
}

void ThreadPool::WorkerLoop() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      has_task_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      // Shutdown drains the queue: leave only when nothing is left to run.
      if (stopping_ && tasks_.empty()) {
        return;
      }
      task = std::move(tasks_.front());
      tasks_.pop();
    }

    // A failing task must not terminate the worker thread.
    try {
      task();
    } catch (const std::exception& error) {
      LogTaskFailure(std::string("ThreadPool: task threw: ") + error.what());
    } catch (...) {
      LogTaskFailure("ThreadPool: task threw unknown exception");
    }
  }
}

}  // namespace search
