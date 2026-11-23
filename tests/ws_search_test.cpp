#include <crow.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <semaphore>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/inverted_index.h"
#include "core/search_engine.h"
#include "services/index_service.h"
#include "services/search_pipeline.h"
#include "services/search_socket.h"
#include "test_support.h"

namespace {

using namespace std::chrono_literals;

using search::IndexService;
using search::InvertedIndex;
using search::SearchEngine;
using search::SearchPipeline;
using search::SearchRunner;
using search::SearchSocketOptions;
using search::SearchSocketService;
using search::test_support::BuildGate;
using search::test_support::TempDir;

// Records what the service sends instead of writing to a socket. The service
// runs the pipeline on a pool thread, so the recorder is thread-safe and can
// wait for an expected number of messages instead of sleeping.
class RecordingConnection : public crow::websocket::connection {
 public:
  void send_binary(std::string /*message*/) override {}
  void send_text(std::string message) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      sent_.push_back(std::move(message));
    }
    condition_.notify_all();
  }
  void send_ping(std::string /*message*/) override {}
  void send_pong(std::string /*message*/) override {}
  void close(std::string const& /*message*/,
             std::uint16_t /*status_code*/) override {
    closed_.store(true);
  }
  std::string get_remote_ip() override { return "127.0.0.1"; }
  std::string get_subprotocol() const override { return {}; }

  bool WaitForCount(std::size_t count,
                    std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout,
                               [this, count] { return sent_.size() >= count; });
  }

  std::size_t Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sent_.size();
  }

  bool closed() const { return closed_.load(); }

  std::vector<nlohmann::json> Messages() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<nlohmann::json> messages;
    messages.reserve(sent_.size());
    for (const std::string& message : sent_) {
      messages.push_back(nlohmann::json::parse(message));
    }
    return messages;
  }

 private:
  mutable std::mutex mutex_;
  mutable std::condition_variable condition_;
  std::vector<std::string> sent_;
  std::atomic<bool> closed_{false};
};

// Expander that turns every query into the given terms.
search::QueryExpander FixedExpander(std::vector<std::string> terms) {
  return [terms = std::move(terms)](std::string_view) { return terms; };
}

}  // namespace

TEST(SearchSocket, SendsExpansionSearchAndDoneInOrder) {
  TempDir dir;
  dir.Write("a.txt", "fish");
  dir.Write("b.txt", "whale");
  IndexService service(dir.path().string());
  ASSERT_TRUE(service.Build(2).has_value());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine, FixedExpander({"whale"}));
  SearchSocketService socket_service(service, pipeline);

  RecordingConnection connection;
  socket_service.OnOpen(connection);
  socket_service.OnMessage(connection, R"({"query": "fish"})");

  ASSERT_TRUE(connection.WaitForCount(3, 5s));
  EXPECT_FALSE(connection.closed());
  const std::vector<nlohmann::json> messages = connection.Messages();
  ASSERT_EQ(3U, messages.size());

  EXPECT_EQ("query_expansion", messages[0].at("stage"));
  EXPECT_EQ("fish", messages[0].at("data").at("original"));
  ASSERT_EQ(1U, messages[0].at("data").at("expanded_terms").size());
  EXPECT_EQ("whale", messages[0].at("data").at("expanded_terms").at(0));

  EXPECT_EQ("keyword_search", messages[1].at("stage"));
  const nlohmann::json& results = messages[1].at("data").at("results");
  ASSERT_EQ(2U, results.size());
  EXPECT_EQ("a.txt", results.at(0).at("doc_id"));
  EXPECT_EQ("b.txt", results.at(1).at("doc_id"));
  EXPECT_GE(messages[1].at("data").at("time_ms").get<long long>(), 0);
  EXPECT_EQ(3U, results.at(0).size());

  EXPECT_EQ("done", messages[2].at("stage"));
  EXPECT_FALSE(messages[2].contains("data"));

  socket_service.Shutdown();
}

TEST(SearchSocket, SearchesTheOriginalQueryWithoutExpansion) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  IndexService service(dir.path().string());
  ASSERT_TRUE(service.Build(1).has_value());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);
  SearchSocketService socket_service(service, pipeline);

  RecordingConnection connection;
  socket_service.OnOpen(connection);
  socket_service.OnMessage(connection, R"({"query": "alpha"})");

  ASSERT_TRUE(connection.WaitForCount(3, 5s));
  const std::vector<nlohmann::json> messages = connection.Messages();
  EXPECT_TRUE(messages[0].at("data").at("expanded_terms").is_array());
  EXPECT_TRUE(messages[0].at("data").at("expanded_terms").empty());
  EXPECT_EQ("alpha", messages[0].at("data").at("original"));
  ASSERT_EQ(1U, messages[1].at("data").at("results").size());
  EXPECT_EQ("a.txt", messages[1].at("data").at("results").at(0).at("doc_id"));
  EXPECT_EQ("done", messages[2].at("stage"));

  socket_service.Shutdown();
}

// Regression: the WebSocket route used to run the whole pipeline first, so the
// client saw query_expansion only after keyword_search had finished. Here the
// search is paused and the expansion must already be delivered.
TEST(SearchSocket, DeliversExpansionWhileKeywordSearchIsStillRunning) {
  TempDir dir;
  dir.Write("a.txt", "fish");
  dir.Write("b.txt", "whale");
  IndexService service(dir.path().string());
  ASSERT_TRUE(service.Build(1).has_value());
  const SearchEngine engine(dir.path().string());

  std::counting_semaphore<64> search_started{0};
  std::counting_semaphore<64> release_search{0};
  SearchRunner runner = [&](const InvertedIndex& index,
                            std::string_view query) {
    search_started.release();
    release_search.acquire();
    return engine.Search(index, query);
  };
  const SearchPipeline pipeline(engine, FixedExpander({"whale"}), runner);
  SearchSocketOptions options;
  options.workers = 1;
  SearchSocketService socket_service(service, pipeline, options);

  RecordingConnection connection;
  socket_service.OnOpen(connection);
  socket_service.OnMessage(connection, R"({"query": "fish"})");
  search_started.acquire();

  ASSERT_TRUE(connection.WaitForCount(1, 5s));
  EXPECT_EQ(1U, connection.Count());
  EXPECT_EQ("query_expansion", connection.Messages().at(0).at("stage"));

  release_search.release();
  ASSERT_TRUE(connection.WaitForCount(3, 5s));
  const std::vector<nlohmann::json> messages = connection.Messages();
  EXPECT_EQ("keyword_search", messages[1].at("stage"));
  EXPECT_EQ("done", messages[2].at("stage"));

  socket_service.Shutdown();
}

TEST(SearchSocket, ReportsInvalidJsonWithoutClosing) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  IndexService service(dir.path().string());
  ASSERT_TRUE(service.Build(1).has_value());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);
  SearchSocketService socket_service(service, pipeline);

  RecordingConnection connection;
  socket_service.OnOpen(connection);
  socket_service.OnMessage(connection, "not json");

  ASSERT_TRUE(connection.WaitForCount(1, 5s));
  EXPECT_FALSE(connection.closed());
  const nlohmann::json message = connection.Messages().at(0);
  EXPECT_EQ("error", message.at("stage"));
  EXPECT_EQ("invalid JSON body", message.at("data").at("message"));

  socket_service.Shutdown();
}

TEST(SearchSocket, ReportsAnEmptyQuery) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  IndexService service(dir.path().string());
  ASSERT_TRUE(service.Build(1).has_value());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);
  SearchSocketService socket_service(service, pipeline);

  RecordingConnection connection;
  socket_service.OnOpen(connection);
  socket_service.OnMessage(connection, R"({"query": ""})");

  ASSERT_TRUE(connection.WaitForCount(1, 5s));
  EXPECT_EQ("query must be a non-empty string",
            connection.Messages().at(0).at("data").at("message"));

  socket_service.Shutdown();
}

TEST(SearchSocket, ReportsAnUnbuiltIndex) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  IndexService service(dir.path().string());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);
  SearchSocketService socket_service(service, pipeline);

  RecordingConnection connection;
  socket_service.OnOpen(connection);
  socket_service.OnMessage(connection, R"({"query": "alpha"})");

  ASSERT_TRUE(connection.WaitForCount(1, 5s));
  EXPECT_FALSE(connection.closed());
  EXPECT_EQ("error", connection.Messages().at(0).at("stage"));
  EXPECT_EQ("index not built",
            connection.Messages().at(0).at("data").at("message"));

  socket_service.Shutdown();
}

TEST(SearchSocket, ReportsABuildingIndex) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  BuildGate gate("whale");
  gate.Hold("whale");
  IndexService service(
      dir.path().string(),
      [&gate](const std::filesystem::path& path) { return gate.Read(path); });
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);
  SearchSocketService socket_service(service, pipeline);

  std::thread builder([&service] { service.Build(1); });
  gate.WaitUntilReading();

  RecordingConnection connection;
  socket_service.OnOpen(connection);
  socket_service.OnMessage(connection, R"({"query": "whale"})");

  gate.Release();
  builder.join();

  ASSERT_TRUE(connection.WaitForCount(1, 5s));
  EXPECT_EQ("index is building",
            connection.Messages().at(0).at("data").at("message"));

  socket_service.Shutdown();
}

TEST(SearchSocket, HandlesAnotherQueryAfterAnError) {
  TempDir dir;
  dir.Write("a.txt", "zebra");
  IndexService service(dir.path().string());
  ASSERT_TRUE(service.Build(1).has_value());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);
  SearchSocketService socket_service(service, pipeline);

  RecordingConnection connection;
  socket_service.OnOpen(connection);
  socket_service.OnMessage(connection, "not json");
  ASSERT_TRUE(connection.WaitForCount(1, 5s));
  socket_service.OnMessage(connection, R"({"query": "zebra"})");

  ASSERT_TRUE(connection.WaitForCount(4, 5s));
  const std::vector<nlohmann::json> messages = connection.Messages();
  EXPECT_EQ("error", messages[0].at("stage"));
  EXPECT_EQ("query_expansion", messages[1].at("stage"));
  EXPECT_EQ("keyword_search", messages[2].at("stage"));
  EXPECT_EQ("done", messages[3].at("stage"));

  socket_service.Shutdown();
}

// One worker plus one queue slot per connection: the third message of the same
// connection has nowhere to go and must be answered with a single busy error.
TEST(SearchSocket, RejectsAQueryWhenThePerConnectionQueueIsFull) {
  TempDir dir;
  dir.Write("a.txt", "fish");
  IndexService service(dir.path().string());
  ASSERT_TRUE(service.Build(1).has_value());
  const SearchEngine engine(dir.path().string());

  std::counting_semaphore<64> search_started{0};
  std::counting_semaphore<64> release_search{0};
  SearchRunner runner = [&](const InvertedIndex& index,
                            std::string_view query) {
    search_started.release();
    release_search.acquire();
    return engine.Search(index, query);
  };
  const SearchPipeline pipeline(engine, FixedExpander({"whale"}), runner);
  SearchSocketOptions options;
  options.workers = 1;
  options.task_queue_capacity = 8;
  options.per_connection_queue = 1;
  SearchSocketService socket_service(service, pipeline, options);

  RecordingConnection connection;
  socket_service.OnOpen(connection);
  socket_service.OnMessage(connection, R"({"query": "fish"})");
  search_started.acquire();
  socket_service.OnMessage(connection, R"({"query": "fish"})");
  socket_service.OnMessage(connection, R"({"query": "fish"})");

  ASSERT_TRUE(connection.WaitForCount(2, 5s));
  const std::vector<nlohmann::json> messages = connection.Messages();
  EXPECT_EQ("query_expansion", messages[0].at("stage"));
  EXPECT_EQ("error", messages[1].at("stage"));
  EXPECT_EQ("server is busy", messages[1].at("data").at("message"));

  release_search.release(2);
  socket_service.Shutdown();
}

// The pool queue itself is bounded: with one worker busy and one waiting slot,
// a task for a third connection is rejected instead of growing memory.
TEST(SearchSocket, ReportsBusyWhenThePoolQueueIsFull) {
  TempDir dir;
  dir.Write("a.txt", "fish");
  IndexService service(dir.path().string());
  ASSERT_TRUE(service.Build(1).has_value());
  const SearchEngine engine(dir.path().string());

  std::counting_semaphore<64> search_started{0};
  std::counting_semaphore<64> release_search{0};
  SearchRunner runner = [&](const InvertedIndex& index,
                            std::string_view query) {
    search_started.release();
    release_search.acquire();
    return engine.Search(index, query);
  };
  const SearchPipeline pipeline(engine, FixedExpander({"whale"}), runner);
  SearchSocketOptions options;
  options.workers = 1;
  options.task_queue_capacity = 1;
  options.per_connection_queue = 4;
  SearchSocketService socket_service(service, pipeline, options);

  RecordingConnection first;
  RecordingConnection second;
  RecordingConnection third;
  socket_service.OnOpen(first);
  socket_service.OnOpen(second);
  socket_service.OnOpen(third);

  socket_service.OnMessage(first, R"({"query": "fish"})");
  search_started.acquire();
  socket_service.OnMessage(second, R"({"query": "fish"})");
  socket_service.OnMessage(third, R"({"query": "fish"})");

  ASSERT_TRUE(third.WaitForCount(1, 5s));
  EXPECT_EQ("error", third.Messages().at(0).at("stage"));
  EXPECT_EQ("server is busy", third.Messages().at(0).at("data").at("message"));

  release_search.release(2);
  socket_service.Shutdown();
}

// After OnClose the session is marked closed before Crow can destroy the
// connection. A task that is still running must finish without sending to the
// dead connection.
TEST(SearchSocket, StopsSendingAfterTheClientDisconnects) {
  TempDir dir;
  dir.Write("a.txt", "fish");
  dir.Write("b.txt", "whale");
  IndexService service(dir.path().string());
  ASSERT_TRUE(service.Build(1).has_value());
  const SearchEngine engine(dir.path().string());

  std::counting_semaphore<64> search_started{0};
  std::counting_semaphore<64> release_search{0};
  SearchRunner runner = [&](const InvertedIndex& index,
                            std::string_view query) {
    search_started.release();
    release_search.acquire();
    return engine.Search(index, query);
  };
  const SearchPipeline pipeline(engine, FixedExpander({"whale"}), runner);
  SearchSocketOptions options;
  options.workers = 1;
  SearchSocketService socket_service(service, pipeline, options);

  RecordingConnection connection;
  socket_service.OnOpen(connection);
  socket_service.OnMessage(connection, R"({"query": "fish"})");
  search_started.acquire();
  ASSERT_TRUE(connection.WaitForCount(1, 5s));

  socket_service.OnClose(connection);
  release_search.release();

  // A second connection is served only after the first task has fully returned
  // (one worker), which makes the count check below deterministic.
  RecordingConnection other;
  socket_service.OnOpen(other);
  socket_service.OnMessage(other, R"({"query": "whale"})");
  release_search.release();
  ASSERT_TRUE(other.WaitForCount(3, 5s));

  EXPECT_EQ(1U, connection.Count());

  socket_service.Shutdown();
}

TEST(SearchSocket, RejectsMessagesAfterShutdown) {
  TempDir dir;
  dir.Write("a.txt", "fish");
  IndexService service(dir.path().string());
  ASSERT_TRUE(service.Build(1).has_value());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine, FixedExpander({"whale"}));
  SearchSocketService socket_service(service, pipeline);

  RecordingConnection connection;
  socket_service.OnOpen(connection);
  socket_service.Shutdown();
  socket_service.OnMessage(connection, R"({"query": "fish"})");

  ASSERT_TRUE(connection.WaitForCount(1, 5s));
  EXPECT_EQ("error", connection.Messages().at(0).at("stage"));
  EXPECT_EQ("server is shutting down",
            connection.Messages().at(0).at("data").at("message"));
}

// Several messages sent back-to-back on one connection are answered in FIFO
// order, even though the work runs on a pool thread.
TEST(SearchSocket, ValidationErrorWaitsForTheActiveQuery) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  IndexService service(dir.path().string());
  ASSERT_TRUE(service.Build(1));
  const SearchEngine engine(dir.path().string());
  std::counting_semaphore<64> entered{0}, release{0};
  const SearchPipeline pipeline(engine, {}, [&](const auto& index, auto query) {
    entered.release();
    release.acquire();
    return engine.Search(index, query);
  });
  SearchSocketService sockets(service, pipeline);
  RecordingConnection connection;
  sockets.OnOpen(connection);
  sockets.OnMessage(connection, R"({"query":"alpha"})");
  EXPECT_TRUE(entered.try_acquire_for(5s));
  sockets.OnMessage(connection, "not json");
  EXPECT_EQ(1U, connection.Count());
  release.release();
  EXPECT_TRUE(connection.WaitForCount(4, 5s));
  sockets.Shutdown();
  const auto messages = connection.Messages();
  ASSERT_EQ(4U, messages.size());
  EXPECT_EQ("done", messages[2].at("stage"));
  EXPECT_EQ("error", messages[3].at("stage"));
}

TEST(SearchSocket, KeepsMessagesOfOneConnectionInOrder) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  dir.Write("b.txt", "beta");
  IndexService service(dir.path().string());
  ASSERT_TRUE(service.Build(1).has_value());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);
  SearchSocketService socket_service(service, pipeline);

  RecordingConnection connection;
  socket_service.OnOpen(connection);
  socket_service.OnMessage(connection, R"({"query": "alpha"})");
  socket_service.OnMessage(connection, R"({"query": "beta"})");

  ASSERT_TRUE(connection.WaitForCount(6, 5s));
  const std::vector<nlohmann::json> messages = connection.Messages();
  ASSERT_EQ(6U, messages.size());
  EXPECT_EQ("alpha", messages[0].at("data").at("original"));
  EXPECT_EQ("done", messages[2].at("stage"));
  EXPECT_EQ("beta", messages[3].at("data").at("original"));
  EXPECT_EQ("done", messages[5].at("stage"));

  socket_service.Shutdown();
}
