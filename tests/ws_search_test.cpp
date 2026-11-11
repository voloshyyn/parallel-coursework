#include <gtest/gtest.h>

#include <crow.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "api.h"
#include "core/search_engine.h"
#include "services/index_service.h"
#include "services/search_pipeline.h"
#include "test_support.h"

namespace {

using namespace std::chrono_literals;

using search::IndexService;
using search::SearchEngine;
using search::SearchPipeline;
using search::test_support::BuildGate;
using search::test_support::TempDir;

// Records what the server sends instead of writing to a socket, so the message
// protocol can be tested without a network.
class RecordingConnection : public crow::websocket::connection {
 public:
  void send_binary(std::string /*message*/) override {}
  void send_text(std::string message) override {
    sent_.push_back(std::move(message));
  }
  void send_ping(std::string /*message*/) override {}
  void send_pong(std::string /*message*/) override {}
  void close(std::string const& /*message*/,
             std::uint16_t /*status_code*/) override {
    closed_ = true;
  }
  std::string get_remote_ip() override { return "127.0.0.1"; }
  std::string get_subprotocol() const override { return {}; }

  const std::vector<std::string>& sent() const { return sent_; }
  bool closed() const { return closed_; }

  std::vector<nlohmann::json> Messages() const {
    std::vector<nlohmann::json> messages;
    for (const std::string& message : sent_) {
      messages.push_back(nlohmann::json::parse(message));
    }
    return messages;
  }

 private:
  std::vector<std::string> sent_;
  bool closed_ = false;
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

  RecordingConnection connection;
  search::HandleSearchSocketMessage(connection, service, pipeline,
                                    R"({"query": "fish"})");

  ASSERT_EQ(3U, connection.sent().size());
  EXPECT_FALSE(connection.closed());
  const std::vector<nlohmann::json> messages = connection.Messages();

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
  // A hit keeps exactly the documented fields.
  EXPECT_EQ(3U, results.at(0).size());

  EXPECT_EQ("done", messages[2].at("stage"));
  // docs/api.md shows done without a data field.
  EXPECT_FALSE(messages[2].contains("data"));
}

TEST(SearchSocket, SearchesTheOriginalQueryWithoutExpansion) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  IndexService service(dir.path().string());
  ASSERT_TRUE(service.Build(1).has_value());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);

  RecordingConnection connection;
  search::HandleSearchSocketMessage(connection, service, pipeline,
                                    R"({"query": "alpha"})");

  ASSERT_EQ(3U, connection.sent().size());
  const std::vector<nlohmann::json> messages = connection.Messages();
  EXPECT_TRUE(messages[0].at("data").at("expanded_terms").is_array());
  EXPECT_TRUE(messages[0].at("data").at("expanded_terms").empty());
  EXPECT_EQ("alpha", messages[0].at("data").at("original"));
  ASSERT_EQ(1U, messages[1].at("data").at("results").size());
  EXPECT_EQ("a.txt", messages[1].at("data").at("results").at(0).at("doc_id"));
  EXPECT_EQ("done", messages[2].at("stage"));
}

TEST(SearchSocket, ReportsInvalidJsonWithoutClosing) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  IndexService service(dir.path().string());
  ASSERT_TRUE(service.Build(1).has_value());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);

  RecordingConnection connection;
  search::HandleSearchSocketMessage(connection, service, pipeline, "not json");

  ASSERT_EQ(1U, connection.sent().size());
  EXPECT_FALSE(connection.closed());
  const nlohmann::json message = connection.Messages().at(0);
  EXPECT_EQ("error", message.at("stage"));
  EXPECT_EQ("invalid JSON body", message.at("data").at("message"));
}

TEST(SearchSocket, ReportsAnEmptyQuery) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  IndexService service(dir.path().string());
  ASSERT_TRUE(service.Build(1).has_value());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);

  RecordingConnection connection;
  search::HandleSearchSocketMessage(connection, service, pipeline,
                                    R"({"query": ""})");

  ASSERT_EQ(1U, connection.sent().size());
  EXPECT_EQ("query must be a non-empty string",
            connection.Messages().at(0).at("data").at("message"));
}

TEST(SearchSocket, ReportsAnUnbuiltIndex) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  IndexService service(dir.path().string());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);

  RecordingConnection connection;
  search::HandleSearchSocketMessage(connection, service, pipeline,
                                    R"({"query": "alpha"})");

  ASSERT_EQ(1U, connection.sent().size());
  EXPECT_FALSE(connection.closed());
  EXPECT_EQ("error", connection.Messages().at(0).at("stage"));
  EXPECT_EQ("index not built",
            connection.Messages().at(0).at("data").at("message"));
}

TEST(SearchSocket, ReportsABuildingIndex) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  // The gate holds the build inside the reader, so the answer is observed
  // deterministically instead of by racing a fast build.
  BuildGate gate("whale");
  gate.Hold("whale");
  IndexService service(dir.path().string(),
                       [&gate](const std::filesystem::path& path) {
                         return gate.Read(path);
                       });
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);

  std::thread builder([&service] { service.Build(1); });
  gate.WaitUntilReading();

  RecordingConnection connection;
  search::HandleSearchSocketMessage(connection, service, pipeline,
                                    R"({"query": "whale"})");

  gate.Release();
  builder.join();

  ASSERT_EQ(1U, connection.sent().size());
  EXPECT_EQ("index is building",
            connection.Messages().at(0).at("data").at("message"));
}

TEST(SearchSocket, HandlesAnotherQueryAfterAnError) {
  TempDir dir;
  dir.Write("a.txt", "zebra");
  IndexService service(dir.path().string());
  ASSERT_TRUE(service.Build(1).has_value());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);

  RecordingConnection connection;
  search::HandleSearchSocketMessage(connection, service, pipeline, "not json");
  search::HandleSearchSocketMessage(connection, service, pipeline,
                                    R"({"query": "zebra"})");

  ASSERT_EQ(4U, connection.sent().size());
  const std::vector<nlohmann::json> messages = connection.Messages();
  EXPECT_EQ("error", messages[0].at("stage"));
  EXPECT_EQ("query_expansion", messages[1].at("stage"));
  EXPECT_EQ("keyword_search", messages[2].at("stage"));
  EXPECT_EQ("done", messages[3].at("stage"));
}
