#include "test_support.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using search::test_support::TempDir;

std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
}

}  // namespace

TEST(TempDir, CreatesUniqueDirectoriesAndRemovesThem) {
  std::vector<std::unique_ptr<TempDir>> directories;
  std::set<std::filesystem::path> paths;
  for (int i = 0; i < 32; ++i) {
    directories.push_back(std::make_unique<TempDir>());
    EXPECT_TRUE(std::filesystem::is_directory(directories.back()->path()));
    paths.insert(directories.back()->path());
  }
  EXPECT_EQ(32U, paths.size());

  const std::vector<std::filesystem::path> removed(paths.begin(), paths.end());
  directories.clear();
  for (const std::filesystem::path& path : removed) {
    EXPECT_FALSE(std::filesystem::exists(path)) << path;
  }
}

// The check runs inside its own safe temporary directory, so nothing outside
// the test is touched even if the assertion fails.
TEST(TempDir, DoesNotRemoveOrChangeAnExistingDirectory) {
  const TempDir outer;
  const std::filesystem::path existing = outer.path() / "existing";
  ASSERT_TRUE(std::filesystem::create_directory(existing));
  {
    std::ofstream control(existing / "control.txt", std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(control));
    control << "keep me";
  }

  {
    const TempDir inner;
    EXPECT_NE(inner.path(), existing);
    EXPECT_NE(outer.path(), inner.path());
  }

  EXPECT_TRUE(std::filesystem::is_directory(existing));
  EXPECT_EQ("keep me", ReadAll(existing / "control.txt"));
}

TEST(TempDir, IsSafeUnderConcurrentCreation) {
  std::mutex paths_mutex;
  std::set<std::filesystem::path> paths;
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 8; ++worker) {
    workers.emplace_back([&paths_mutex, &paths] {
      for (int i = 0; i < 16; ++i) {
        const TempDir directory;
        // Every directory must still exist while its owner is alive, even
        // when another thread creates and removes its own directory at the
        // same time.
        EXPECT_TRUE(std::filesystem::is_directory(directory.path()));
        {
          std::ofstream marker(directory.path() / "marker.txt",
                               std::ios::binary);
          EXPECT_TRUE(static_cast<bool>(marker));
          marker << directory.path().string();
        }
        {
          std::lock_guard<std::mutex> lock(paths_mutex);
          paths.insert(directory.path());
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  EXPECT_EQ(128U, paths.size());
}
