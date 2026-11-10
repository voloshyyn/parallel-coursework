#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace search::test_support {

// Temporary directory with a helper for writing documents. Removed on
// destruction, so tests never leave dataset-like files behind.
class TempDir {
 public:
  TempDir() {
    static std::atomic<int> counter{0};
    path_ = std::filesystem::temp_directory_path() /
            ("search_test_" + std::to_string(counter.fetch_add(1)));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
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

}  // namespace search::test_support
