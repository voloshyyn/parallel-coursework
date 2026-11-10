#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace search {

// Probes Ollama in the background on a fixed interval and keeps the latest
// result in an atomic flag, so request handlers never wait for the network.
class OllamaMonitor {
 public:
  using Probe = std::function<bool()>;

  OllamaMonitor(Probe probe, std::chrono::milliseconds interval);

  ~OllamaMonitor();

  OllamaMonitor(const OllamaMonitor&) = delete;
  OllamaMonitor& operator=(const OllamaMonitor&) = delete;

  // Starts the background thread. The first probe runs immediately, so the
  // flag is refreshed without waiting for a full interval. Calling Start on a
  // running monitor does nothing.
  void Start();

  // Stops the background thread and waits for the in-flight probe to finish.
  void Stop();

  // Result of the most recent probe. Never blocks and never throws.
  bool IsAvailable() const noexcept {
    return available_.load(std::memory_order_relaxed);
  }

 private:
  void Loop();

  Probe probe_;
  std::chrono::milliseconds interval_;
  std::atomic<bool> available_{false};
  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable wakeup_;
  bool stop_ = false;
};

}  // namespace search
