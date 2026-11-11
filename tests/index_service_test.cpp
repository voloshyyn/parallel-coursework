#include "services/index_service.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "test_support.h"

namespace {

using search::BuildStats;
using search::IndexService;
using search::IndexState;
using search::PublishedIndex;
using search::test_support::BuildGate;
using search::test_support::TempDir;

}  // namespace

TEST(IndexService, StartsEmpty) {
  TempDir dir;
  const IndexService service(dir.path().string());

  EXPECT_EQ(IndexState::kEmpty, service.State());
  const PublishedIndex published = service.Snapshot();
  EXPECT_EQ(nullptr, published.index);
  EXPECT_EQ(0U, published.stats.documents_indexed);
  EXPECT_EQ(0, published.stats.build_time_ms);
  EXPECT_EQ(0U, published.stats.threads_used);
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

  const PublishedIndex published = service.Snapshot();
  ASSERT_NE(nullptr, published.index);
  EXPECT_EQ(2U, published.index->DocumentCount());
  EXPECT_EQ(2U, published.index->Lookup("whale").size());
  EXPECT_EQ(1U, published.index->Lookup("alpha").size());
  EXPECT_EQ(2U, published.stats.documents_indexed);
  EXPECT_EQ(2U, published.stats.threads_used);
}

TEST(IndexService, RebuildReplacesPublishedIndex) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  dir.Write("b.txt", "beta");
  IndexService service(dir.path().string());
  ASSERT_TRUE(service.Build(1).has_value());
  ASSERT_EQ(2U, service.Snapshot().index->DocumentCount());

  std::filesystem::remove(dir.path() / "b.txt");
  const std::optional<BuildStats> stats = service.Build(1);

  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(1U, stats->documents_indexed);
  EXPECT_EQ(1U, service.Snapshot().index->DocumentCount());
  EXPECT_EQ(0U, service.Snapshot().index->Lookup("beta").size());
}

// A rebuild publishes a different number of documents and a different number of
// threads, so a reader that mixed two generations would be caught.
TEST(IndexService, SequentialBuildsPublishIndexAndStatsTogether) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  dir.Write("b.txt", "beta");
  dir.Write("c.txt", "gamma");
  IndexService service(dir.path().string());

  ASSERT_TRUE(service.Build(2).has_value());
  const PublishedIndex first = service.Snapshot();
  ASSERT_NE(nullptr, first.index);
  EXPECT_EQ(3U, first.index->DocumentCount());
  EXPECT_EQ(3U, first.stats.documents_indexed);
  EXPECT_EQ(2U, first.stats.threads_used);

  std::filesystem::remove(dir.path() / "b.txt");
  std::filesystem::remove(dir.path() / "c.txt");
  ASSERT_TRUE(service.Build(5).has_value());

  const PublishedIndex second = service.Snapshot();
  ASSERT_NE(nullptr, second.index);
  EXPECT_EQ(1U, second.index->DocumentCount());
  EXPECT_EQ(1U, second.stats.documents_indexed);
  EXPECT_EQ(5U, second.stats.threads_used);
  EXPECT_NE(first.index, second.index);
}

TEST(IndexService, RejectsSecondBuildWhileFirstRuns) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  dir.Write("b.txt", "beta");

  BuildGate gate("sample text");
  IndexService service(dir.path().string(),
                       [&gate](const std::filesystem::path& path) {
                         return gate.Read(path);
                       });
  gate.Hold("sample text");

  std::optional<BuildStats> first;
  std::thread builder([&service, &first] { first = service.Build(1); });
  gate.WaitUntilReading();

  EXPECT_EQ(IndexState::kBuilding, service.State());
  EXPECT_FALSE(service.Build(1).has_value());
  // The rejected build must not have touched the published generation.
  EXPECT_EQ(nullptr, service.Snapshot().index);

  gate.Release();
  builder.join();

  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(2U, first->documents_indexed);
  EXPECT_EQ(IndexState::kReady, service.State());
}

// 409 while rebuilding: the previous generation keeps answering, the second
// index request is rejected, and only the finished build replaces the index.
TEST(IndexService, RejectsConcurrentBuildWhilePublishedIndexIsRebuilt) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  dir.Write("b.txt", "beta");

  BuildGate gate("alpha");
  IndexService service(dir.path().string(),
                       [&gate](const std::filesystem::path& path) {
                         return gate.Read(path);
                       });
  ASSERT_TRUE(service.Build(1).has_value());
  const PublishedIndex before = service.Snapshot();
  ASSERT_NE(nullptr, before.index);
  EXPECT_EQ(2U, before.index->Lookup("alpha").size());

  gate.Hold("whale whale");
  std::optional<BuildStats> second;
  std::thread rebuilder([&service, &second] { second = service.Build(2); });
  gate.WaitUntilReading();

  EXPECT_EQ(IndexState::kBuilding, service.State());
  EXPECT_FALSE(service.Build(1).has_value());

  // While the rebuild runs, readers keep the complete previous generation.
  const PublishedIndex during = service.Snapshot();
  EXPECT_EQ(before.index, during.index);
  EXPECT_EQ(before.stats.documents_indexed, during.stats.documents_indexed);
  EXPECT_EQ(before.stats.threads_used, during.stats.threads_used);

  gate.Release();
  rebuilder.join();

  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(IndexState::kReady, service.State());
  const PublishedIndex after = service.Snapshot();
  ASSERT_NE(nullptr, after.index);
  EXPECT_NE(before.index, after.index);
  EXPECT_EQ(0U, after.index->Lookup("alpha").size());
  EXPECT_EQ(2U, after.index->Lookup("whale").size());
  EXPECT_EQ(2U, after.stats.documents_indexed);
  EXPECT_EQ(2U, after.stats.threads_used);
}

TEST(IndexService, KeepsPublishedGenerationWhenRebuildFails) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  std::atomic<int> reads{0};
  IndexService service(dir.path().string(),
                       [&reads](const std::filesystem::path&) {
                         if (reads.fetch_add(1) > 0) {
                           throw std::runtime_error("disk exploded");
                         }
                         return std::string("alpha");
                       });

  ASSERT_TRUE(service.Build(1).has_value());
  const PublishedIndex before = service.Snapshot();

  EXPECT_THROW(service.Build(1), std::runtime_error);

  EXPECT_EQ(IndexState::kReady, service.State());
  const PublishedIndex after = service.Snapshot();
  EXPECT_EQ(before.index, after.index);
  EXPECT_EQ(before.stats.documents_indexed, after.stats.documents_indexed);
  EXPECT_EQ(before.stats.threads_used, after.stats.threads_used);
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
  EXPECT_EQ(nullptr, service.Snapshot().index);
}

// A failed build must not leave the service in the building state forever.
TEST(IndexService, BuildSucceedsAfterFailedFirstBuild) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  std::atomic<bool> fail{true};
  IndexService service(dir.path().string(),
                       [&fail](const std::filesystem::path&) {
                         if (fail.exchange(false)) {
                           throw std::runtime_error("disk exploded");
                         }
                         return std::string("alpha");
                       });

  EXPECT_THROW(service.Build(1), std::runtime_error);
  EXPECT_EQ(IndexState::kEmpty, service.State());

  ASSERT_TRUE(service.Build(1).has_value());
  EXPECT_EQ(IndexState::kReady, service.State());
  ASSERT_NE(nullptr, service.Snapshot().index);
  EXPECT_EQ(1U, service.Snapshot().index->DocumentCount());
}

TEST(IndexService, FailsWhenDirectoryIsMissing) {
  IndexService service("/nonexistent-search-data-dir");

  EXPECT_THROW(service.Build(1), std::runtime_error);
  EXPECT_EQ(IndexState::kEmpty, service.State());
}

TEST(IndexService, RejectsZeroThreads) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  IndexService service(dir.path().string());

  EXPECT_THROW(service.Build(0), std::invalid_argument);
  EXPECT_EQ(IndexState::kEmpty, service.State());

  // The rejected call must not have marked the service as building.
  ASSERT_TRUE(service.Build(1).has_value());
  EXPECT_EQ(IndexState::kReady, service.State());
}
