#include "services/index_service.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <thread>

#include "test_support.h"

namespace {

using namespace std::chrono_literals;

using search::BuildStats;
using search::IndexService;
using search::IndexState;
using search::test_support::TempDir;

// Polls the service until it reports a build in progress, so the test never
// relies on a fixed delay.
bool WaitUntilBuilding(const IndexService& service,
                       std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (service.State() == IndexState::kBuilding) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return service.State() == IndexState::kBuilding;
}

}  // namespace

TEST(IndexService, StartsEmpty) {
  TempDir dir;
  const IndexService service(dir.path().string());

  EXPECT_EQ(IndexState::kEmpty, service.State());
  EXPECT_EQ(nullptr, service.Snapshot());
  EXPECT_EQ(0U, service.Stats().documents_indexed);
  EXPECT_EQ(0, service.Stats().build_time_ms);
  EXPECT_EQ(0U, service.Stats().threads_used);
}

TEST(IndexService, BuildsIndexFromDirectory) {
  TempDir dir;
  dir.Write("a.txt", "alpha whale");
  dir.Write("b.txt", "beta whale");
  IndexService service(dir.path().string());

  const std::optional<BuildStats> stats = service.Build(2);

  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(2U, stats->documents_indexed);
  EXPECT_EQ(2U, stats->threads_used);
  EXPECT_GE(stats->build_time_ms, 0);
  EXPECT_EQ(IndexState::kReady, service.State());

  const auto index = service.Snapshot();
  ASSERT_NE(nullptr, index);
  EXPECT_EQ(2U, index->DocumentCount());
  EXPECT_EQ(2U, index->Lookup("whale").size());
  EXPECT_EQ(1U, index->Lookup("alpha").size());
  EXPECT_EQ(2U, service.Stats().documents_indexed);
  EXPECT_EQ(2U, service.Stats().threads_used);
}

TEST(IndexService, RebuildReplacesPublishedIndex) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  dir.Write("b.txt", "beta");
  IndexService service(dir.path().string());
  ASSERT_TRUE(service.Build(1).has_value());
  ASSERT_EQ(2U, service.Snapshot()->DocumentCount());

  std::filesystem::remove(dir.path() / "b.txt");
  const std::optional<BuildStats> stats = service.Build(1);

  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(1U, stats->documents_indexed);
  EXPECT_EQ(1U, service.Snapshot()->DocumentCount());
  EXPECT_EQ(0U, service.Snapshot()->Lookup("beta").size());
}

TEST(IndexService, RejectsSecondBuildWhileFirstRuns) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  dir.Write("b.txt", "beta");

  // The injected reader blocks every build until the gate opens, which keeps
  // the first build in progress deterministically.
  std::binary_semaphore gate{0};
  IndexService service(dir.path().string(), [&gate](const std::filesystem::path&) {
    gate.acquire();
    gate.release();
    return std::string("sample text");
  });

  std::optional<BuildStats> first;
  std::thread builder([&service, &first] { first = service.Build(1); });

  const bool observed_building = WaitUntilBuilding(service, 5s);
  bool second_accepted = false;
  if (observed_building) {
    second_accepted = service.Build(1).has_value();
  }
  gate.release();
  builder.join();

  EXPECT_TRUE(observed_building);
  EXPECT_FALSE(second_accepted);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(2U, first->documents_indexed);
  EXPECT_EQ(IndexState::kReady, service.State());
}

TEST(IndexService, KeepsPublishedIndexWhenRebuildFails) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  std::atomic<int> reads{0};
  IndexService service(dir.path().string(), [&reads](const std::filesystem::path&) {
    if (reads.fetch_add(1) > 0) {
      throw std::runtime_error("disk exploded");
    }
    return std::string("alpha");
  });

  ASSERT_TRUE(service.Build(1).has_value());
  const std::size_t documents_before = service.Snapshot()->DocumentCount();

  EXPECT_THROW(service.Build(1), std::runtime_error);

  EXPECT_EQ(IndexState::kReady, service.State());
  EXPECT_EQ(documents_before, service.Snapshot()->DocumentCount());
  EXPECT_EQ(1U, service.Stats().documents_indexed);
}

TEST(IndexService, ReportsEmptyStateAfterFailedFirstBuild) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  IndexService service(dir.path().string(),
                       [](const std::filesystem::path&) -> std::string {
                         throw std::runtime_error("disk exploded");
                       });

  EXPECT_THROW(service.Build(1), std::runtime_error);

  EXPECT_EQ(IndexState::kEmpty, service.State());
  EXPECT_EQ(nullptr, service.Snapshot());
}

TEST(IndexService, FailsWhenDirectoryIsMissing) {
  IndexService service("/nonexistent-search-data-dir");

  EXPECT_THROW(service.Build(1), std::runtime_error);
  EXPECT_EQ(IndexState::kEmpty, service.State());
}

TEST(IndexService, RejectsZeroThreads) {
  TempDir dir;
  IndexService service(dir.path().string());

  EXPECT_THROW(service.Build(0), std::invalid_argument);
  EXPECT_EQ(IndexState::kEmpty, service.State());
}
