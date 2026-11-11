#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <utility>

namespace search::test_support {
namespace detail {

// Suffix that differs between processes and between instances of one process:
// wall clock, an in-process counter and 64 random bits.
inline std::string UniqueSuffix() {
  static std::atomic<unsigned long long> counter{0};
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::random_device device;
  const auto random_bits =
      (static_cast<unsigned long long>(device()) << 32) ^
      static_cast<unsigned long long>(device());
  return std::to_string(now) + "-" +
         std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + "-" +
         std::to_string(random_bits);
}

}  // namespace detail

// Temporary directory with a helper for writing documents. Removed on
// destruction, so tests never leave dataset-like files behind.
//
// A directory is created with std::filesystem::create_directory(), which is
// atomic: it fails when the path already exists. A name clash therefore retries
// with another suffix instead of deleting or reusing somebody else's directory,
// which keeps `ctest -j` safe. Only a directory this instance created is
// removed, and only on destruction.
class TempDir {
 public:
  TempDir() {
    const std::filesystem::path base =
        std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 32; ++attempt) {
      const std::filesystem::path candidate =
          base / ("search-test-" + detail::UniqueSuffix());
      std::error_code error;
      if (std::filesystem::create_directory(candidate, error)) {
        path_ = candidate;
        return;
      }
    }
    throw std::runtime_error("cannot create a unique temporary directory");
  }

  ~TempDir() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const { return path_; }

  void Write(const std::string& name, const std::string& content) const {
    std::ofstream file(path_ / name, std::ios::binary);
    if (!file) {
      throw std::runtime_error("cannot write " + name);
    }
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
  }

 private:
  std::filesystem::path path_;
};

// Holds an index build inside the document reader, so a test can observe the
// service while a build is running without timing assumptions. The reader is
// only reached after IndexService has marked itself as building.
class BuildGate {
 public:
  explicit BuildGate(std::string text) : text_(std::move(text)) {}

  std::string Read(const std::filesystem::path&) {
    if (hold_.load(std::memory_order_acquire)) {
      if (announced_.exchange(false, std::memory_order_acq_rel)) {
        started_.release();
      }
      gate_.acquire();
      gate_.release();
    }
    return text_;
  }

  // Makes every following read block until Release(); `text` is what the held
  // build indexes. Called before the build starts, never concurrently with it.
  void Hold(std::string text) {
    text_ = std::move(text);
    announced_.store(true, std::memory_order_release);
    hold_.store(true, std::memory_order_release);
  }

  // Blocks until the held build reads its first document.
  void WaitUntilReading() { started_.acquire(); }

  void Release() { gate_.release(); }

 private:
  std::atomic<bool> hold_{false};
  std::atomic<bool> announced_{true};
  std::string text_;
  std::binary_semaphore started_{0};
  std::binary_semaphore gate_{0};
};

}  // namespace search::test_support
