#include "integrations/ollama_monitor.h"

#include <utility>

namespace search {

OllamaMonitor::OllamaMonitor(Probe probe, std::chrono::milliseconds interval)
    : probe_(std::move(probe)), interval_(interval) {}

OllamaMonitor::~OllamaMonitor() { Stop(); }

void OllamaMonitor::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (worker_.joinable()) {
    return;
  }
  stop_ = false;
  worker_ = std::thread(&OllamaMonitor::Loop, this);
}

void OllamaMonitor::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  wakeup_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void OllamaMonitor::Loop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_) {
    // The probe performs network I/O and must not hold the mutex, otherwise
    // Stop would block until the probe times out.
    lock.unlock();
    bool available = false;
    try {
      available = probe_();
    } catch (...) {
      available = false;
    }
    available_.store(available, std::memory_order_relaxed);
    lock.lock();
    wakeup_.wait_for(lock, interval_, [this] { return stop_; });
  }
}

}  // namespace search
