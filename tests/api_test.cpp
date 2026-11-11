#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <crow.h>
#include <nlohmann/json.hpp>

#include "api.h"
#include "core/search_engine.h"
#include "services/index_service.h"
#include "services/search_pipeline.h"
#include "test_support.h"

namespace {

using namespace std::chrono_literals;

using search::IndexService;
using search::IndexState;
using search::SearchEngine;
using search::SearchPipeline;
using search::test_support::BuildGate;
using search::test_support::TempDir;

// Runs the REST search routes on a fixed port for the lifetime of the object.
class ApiServer {
 public:
  ApiServer(IndexService& service, const SearchPipeline& pipeline, int port)
      : port_(port) {
    search::RegisterSearchRoutes(app_, service, pipeline);
    app_.loglevel(crow::LogLevel::Critical);
    app_.bindaddr("127.0.0.1").port(port_).multithreaded();
    worker_ = std::thread([this] { app_.run(); });
  }

  ~ApiServer() {
    app_.stop();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  ApiServer(const ApiServer&) = delete;
  ApiServer& operator=(const ApiServer&) = delete;

  // Waits until the port accepts connections by polling a real endpoint.
  bool WaitUntilReady() {
    httplib::Client client("127.0.0.1", port_);
    client.set_connection_timeout(200ms);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (client.Get("/api/stats")) {
        return true;
      }
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }

  int port() const { return port_; }

 private:
  const int port_;
  crow::SimpleApp app_;
  std::thread worker_;
};

httplib::Client MakeClient(int port) {
  httplib::Client client("127.0.0.1", port);
  client.set_connection_timeout(2s);
  client.set_read_timeout(30s);
  return client;
}

nlohmann::json ParseBody(const httplib::Result& result) {
  return nlohmann::json::parse(result->body);
}

}  // namespace

TEST(SearchApi, RejectsInvalidThreads) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  IndexService service(dir.path().string());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);
  ApiServer server(service, pipeline, 18090);
  ASSERT_TRUE(server.WaitUntilReady());
  httplib::Client client = MakeClient(server.port());

  const std::vector<std::string> bodies = {
      R"({"threads": 0})",     R"({"threads": 65})",
      R"({"threads": "four"})", R"({"threads": 2.5})",
      R"({"threads": true})",
  };
  for (const std::string& body : bodies) {
    SCOPED_TRACE(body);
    const auto result = client.Post("/api/index", body, "application/json");
    ASSERT_TRUE(result);
    EXPECT_EQ(400, result->status);
    EXPECT_NE(std::string::npos,
              result->body.find("\"message\":\"threads must be in 1..64\""));
  }

  const auto invalid = client.Post("/api/index", "not json", "application/json");
  ASSERT_TRUE(invalid);
  EXPECT_EQ(400, invalid->status);
  EXPECT_NE(std::string::npos, invalid->body.find("\"message\":\"invalid JSON body\""));

  EXPECT_EQ(IndexState::kEmpty, service.State());
}

TEST(SearchApi, IndexesDocumentsAndServesStats) {
  TempDir dir;
  dir.Write("a.txt", "the whale");
  dir.Write("b.txt", "the whale whale");
  IndexService service(dir.path().string());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);
  ApiServer server(service, pipeline, 18091);
  ASSERT_TRUE(server.WaitUntilReady());
  httplib::Client client = MakeClient(server.port());

  const auto indexed = client.Post("/api/index", R"({"threads": 2})",
                                   "application/json");
  ASSERT_TRUE(indexed);
  ASSERT_EQ(200, indexed->status) << indexed->body;
  const nlohmann::json index_body = ParseBody(indexed);
  EXPECT_EQ("ok", index_body.at("status"));
  EXPECT_EQ(2, index_body.at("documents_indexed"));
  EXPECT_TRUE(index_body.at("time_ms").is_number_integer());
  EXPECT_GE(index_body.at("time_ms").get<long long>(), 0);

  const auto stats = client.Get("/api/stats");
  ASSERT_TRUE(stats);
  ASSERT_EQ(200, stats->status);
  const nlohmann::json stats_body = ParseBody(stats);
  EXPECT_EQ(2, stats_body.at("documents"));
  EXPECT_EQ(2, stats_body.at("threads_used"));
  EXPECT_TRUE(stats_body.at("index_build_time_ms").is_number_integer());
  EXPECT_GE(stats_body.at("index_build_time_ms").get<long long>(), 0);
}

TEST(SearchApi, DefaultsToFourThreads) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  IndexService service(dir.path().string());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);
  ApiServer server(service, pipeline, 18092);
  ASSERT_TRUE(server.WaitUntilReady());
  httplib::Client client = MakeClient(server.port());

  const auto indexed =
      client.Post("/api/index", R"({})", "application/json");
  ASSERT_TRUE(indexed);
  ASSERT_EQ(200, indexed->status) << indexed->body;
  EXPECT_EQ(1, ParseBody(indexed).at("documents_indexed"));

  const auto stats = client.Get("/api/stats");
  ASSERT_TRUE(stats);
  EXPECT_EQ(4, ParseBody(stats).at("threads_used"));
}

TEST(SearchApi, SearchBeforeIndexReturns409) {
  TempDir dir;
  IndexService service(dir.path().string());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);
  ApiServer server(service, pipeline, 18093);
  ASSERT_TRUE(server.WaitUntilReady());
  httplib::Client client = MakeClient(server.port());

  const auto result = client.Post("/api/search",
                                  R"({"query": "whale", "mode": "keyword"})",
                                  "application/json");
  ASSERT_TRUE(result);
  EXPECT_EQ(409, result->status);
  EXPECT_NE(std::string::npos,
            result->body.find("\"message\":\"index not built\""));
}

TEST(SearchApi, RejectsInvalidSearchRequests) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  IndexService service(dir.path().string());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);
  ApiServer server(service, pipeline, 18094);
  ASSERT_TRUE(server.WaitUntilReady());
  httplib::Client client = MakeClient(server.port());

  const std::vector<std::pair<std::string, std::string>> cases = {
      {"missing query", R"({"mode": "keyword"})"},
      {"empty query", R"({"query": "", "mode": "keyword"})"},
      {"numeric query", R"({"query": 42, "mode": "keyword"})"},
      {"missing mode", R"({"query": "whale"})"},
      {"unknown mode", R"({"query": "whale", "mode": "fuzzy"})"},
      {"invalid json", "{"},
  };
  for (const auto& [name, body] : cases) {
    SCOPED_TRACE(name);
    const auto result = client.Post("/api/search", body, "application/json");
    ASSERT_TRUE(result);
    EXPECT_EQ(400, result->status) << result->body;
  }
}

TEST(SearchApi, SearchesKeywordModeWithRankedResults) {
  TempDir dir;
  dir.Write("a.txt", "the whale");
  dir.Write("b.txt", "whale whale");
  IndexService service(dir.path().string());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);
  ApiServer server(service, pipeline, 18095);
  ASSERT_TRUE(server.WaitUntilReady());
  httplib::Client client = MakeClient(server.port());
  const auto indexed =
      client.Post("/api/index", R"({"threads": 2})", "application/json");
  ASSERT_TRUE(indexed);
  ASSERT_EQ(200, indexed->status) << indexed->body;

  const auto result = client.Post("/api/search",
                                  R"({"query": "whale", "mode": "keyword"})",
                                  "application/json");
  ASSERT_TRUE(result);
  ASSERT_EQ(200, result->status) << result->body;
  const nlohmann::json body = ParseBody(result);
  EXPECT_GE(body.at("time_ms").get<long long>(), 0);

  const nlohmann::json& hits = body.at("results");
  ASSERT_EQ(2U, hits.size());
  EXPECT_EQ("b.txt", hits.at(0).at("doc_id"));
  EXPECT_DOUBLE_EQ(1.0, hits.at(0).at("score").get<double>());
  EXPECT_EQ("a.txt", hits.at(1).at("doc_id"));
  EXPECT_DOUBLE_EQ(0.5, hits.at(1).at("score").get<double>());
  for (const nlohmann::json& hit : hits) {
    EXPECT_TRUE(hit.at("doc_id").is_string());
    EXPECT_TRUE(hit.at("score").is_number());
    EXPECT_TRUE(hit.at("snippet").is_string());
    EXPECT_NE(std::string::npos, hit.at("snippet").get<std::string>().find("whale"));
  }
}

TEST(SearchApi, SearchDuringBuildReturns409) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  dir.Write("b.txt", "beta");

  // The gate holds the build inside the reader, so the 409 answer is observed
  // deterministically instead of by racing a fast build.
  BuildGate gate("whale whale");
  gate.Hold("whale whale");
  IndexService service(dir.path().string(),
                       [&gate](const std::filesystem::path& path) {
                         return gate.Read(path);
                       });
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);
  ApiServer server(service, pipeline, 18096);
  ASSERT_TRUE(server.WaitUntilReady());

  int build_status = 0;
  std::thread builder([&service, &build_status] {
    httplib::Client client("127.0.0.1", 18096);
    client.set_read_timeout(30s);
    const auto result = client.Post("/api/index", R"({"threads": 1})",
                                    "application/json");
    build_status = result ? result->status : -1;
  });
  gate.WaitUntilReading();

  httplib::Client client = MakeClient(server.port());
  const auto during_build = client.Post(
      "/api/search", R"({"query": "whale", "mode": "keyword"})",
      "application/json");
  // A second POST /api/index during the build is rejected by the same rule.
  const auto second_index =
      client.Post("/api/index", R"({"threads": 1})", "application/json");

  gate.Release();
  builder.join();

  ASSERT_TRUE(during_build);
  EXPECT_EQ(409, during_build->status);
  EXPECT_NE(std::string::npos,
            during_build->body.find("\"message\":\"index is building\""));
  ASSERT_TRUE(second_index);
  EXPECT_EQ(409, second_index->status);
  EXPECT_NE(std::string::npos,
            second_index->body.find("\"message\":\"index is building\""));
  EXPECT_EQ(200, build_status);

  const auto after_build = client.Post(
      "/api/search", R"({"query": "whale", "mode": "keyword"})",
      "application/json");
  ASSERT_TRUE(after_build);
  EXPECT_EQ(200, after_build->status);
  EXPECT_EQ(2U, ParseBody(after_build).at("results").size());
}

// A rebuild of an already published index behaves like the first build: new
// searches get 409, and the old generation is served again if the rebuild
// fails.
TEST(SearchApi, SearchDuringRebuildReturns409) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  dir.Write("b.txt", "beta");

  BuildGate gate("alpha");
  IndexService service(dir.path().string(),
                       [&gate](const std::filesystem::path& path) {
                         return gate.Read(path);
                       });
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);
  ApiServer server(service, pipeline, 18097);
  ASSERT_TRUE(server.WaitUntilReady());
  httplib::Client client = MakeClient(server.port());

  const auto indexed =
      client.Post("/api/index", R"({"threads": 2})", "application/json");
  ASSERT_TRUE(indexed);
  ASSERT_EQ(200, indexed->status) << indexed->body;

  gate.Hold("whale whale");
  std::thread rebuilder([&service] {
    httplib::Client builder_client("127.0.0.1", 18097);
    builder_client.set_read_timeout(30s);
    builder_client.Post("/api/index", R"({"threads": 3})",
                        "application/json");
  });
  gate.WaitUntilReading();

  const auto during_rebuild = client.Post(
      "/api/search", R"({"query": "alpha", "mode": "keyword"})",
      "application/json");
  gate.Release();
  rebuilder.join();

  ASSERT_TRUE(during_rebuild);
  EXPECT_EQ(409, during_rebuild->status);
  EXPECT_NE(std::string::npos,
            during_rebuild->body.find("\"message\":\"index is building\""));

  // The rebuild published a different index and a different thread count.
  const auto stats = client.Get("/api/stats");
  ASSERT_TRUE(stats);
  ASSERT_EQ(200, stats->status);
  EXPECT_EQ(2, ParseBody(stats).at("documents"));
  EXPECT_EQ(3, ParseBody(stats).at("threads_used"));

  const auto after_rebuild = client.Post(
      "/api/search", R"({"query": "whale", "mode": "keyword"})",
      "application/json");
  ASSERT_TRUE(after_rebuild);
  ASSERT_EQ(200, after_rebuild->status) << after_rebuild->body;
  EXPECT_EQ(2U, ParseBody(after_rebuild).at("results").size());

  const auto old_terms = client.Post(
      "/api/search", R"({"query": "alpha", "mode": "keyword"})",
      "application/json");
  ASSERT_TRUE(old_terms);
  EXPECT_EQ(200, old_terms->status);
  EXPECT_TRUE(ParseBody(old_terms).at("results").empty());
}

// documents and threads_used belong to the same build: the first build indexes
// three documents with two threads, the second indexes one document with five.
TEST(SearchApi, StatsMatchTheLastSuccessfulBuild) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  dir.Write("b.txt", "beta");
  dir.Write("c.txt", "gamma");
  IndexService service(dir.path().string());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);
  ApiServer server(service, pipeline, 18098);
  ASSERT_TRUE(server.WaitUntilReady());
  httplib::Client client = MakeClient(server.port());

  const auto first =
      client.Post("/api/index", R"({"threads": 2})", "application/json");
  ASSERT_TRUE(first);
  ASSERT_EQ(200, first->status) << first->body;
  const auto first_stats = client.Get("/api/stats");
  ASSERT_TRUE(first_stats);
  EXPECT_EQ(3, ParseBody(first_stats).at("documents"));
  EXPECT_EQ(2, ParseBody(first_stats).at("threads_used"));

  std::filesystem::remove(dir.path() / "b.txt");
  std::filesystem::remove(dir.path() / "c.txt");
  const auto second =
      client.Post("/api/index", R"({"threads": 5})", "application/json");
  ASSERT_TRUE(second);
  ASSERT_EQ(200, second->status) << second->body;

  const auto stats = client.Get("/api/stats");
  ASSERT_TRUE(stats);
  ASSERT_EQ(200, stats->status);
  const nlohmann::json body = ParseBody(stats);
  EXPECT_EQ(1, body.at("documents"));
  EXPECT_EQ(5, body.at("threads_used"));
  EXPECT_GE(body.at("index_build_time_ms").get<long long>(), 0);
}

// Query expansion reaches the REST search: a synonym finds a document even
// when the original query has no matching token, and the response keeps the
// documented shape (REST does not expose the expanded terms).
TEST(SearchApi, SearchUsesExpandedTerms) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  dir.Write("b.txt", "whale");
  IndexService service(dir.path().string());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine, [](std::string_view) {
    return std::vector<std::string>{"whale"};
  });
  ApiServer server(service, pipeline, 18099);
  ASSERT_TRUE(server.WaitUntilReady());
  httplib::Client client = MakeClient(server.port());

  const auto indexed =
      client.Post("/api/index", R"({"threads": 2})", "application/json");
  ASSERT_TRUE(indexed);
  ASSERT_EQ(200, indexed->status) << indexed->body;

  const auto result = client.Post(
      "/api/search", R"({"query": "big fish", "mode": "keyword"})",
      "application/json");
  ASSERT_TRUE(result);
  ASSERT_EQ(200, result->status) << result->body;
  const nlohmann::json body = ParseBody(result);
  ASSERT_EQ(1U, body.at("results").size());
  EXPECT_EQ("b.txt", body.at("results").at(0).at("doc_id"));
  // The fixed contract has exactly these two top-level fields.
  EXPECT_EQ(2U, body.size());
  EXPECT_EQ(std::string::npos, result->body.find("expanded_terms"));
}

// When expansion yields nothing (Ollama unavailable), the search continues with
// the original query instead of failing.
TEST(SearchApi, SearchFallsBackToOriginalQueryWithoutExpansion) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  IndexService service(dir.path().string());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine, [](std::string_view) {
    return std::vector<std::string>{};
  });
  ApiServer server(service, pipeline, 18100);
  ASSERT_TRUE(server.WaitUntilReady());
  httplib::Client client = MakeClient(server.port());

  const auto indexed =
      client.Post("/api/index", R"({"threads": 1})", "application/json");
  ASSERT_TRUE(indexed);
  ASSERT_EQ(200, indexed->status) << indexed->body;

  const auto found = client.Post(
      "/api/search", R"({"query": "alpha", "mode": "keyword"})",
      "application/json");
  ASSERT_TRUE(found);
  ASSERT_EQ(200, found->status) << found->body;
  const nlohmann::json found_body = ParseBody(found);
  ASSERT_EQ(1U, found_body.at("results").size());
  EXPECT_EQ("a.txt", found_body.at("results").at(0).at("doc_id"));

  const auto missing = client.Post(
      "/api/search", R"({"query": "big fish", "mode": "keyword"})",
      "application/json");
  ASSERT_TRUE(missing);
  EXPECT_EQ(200, missing->status);
  EXPECT_TRUE(ParseBody(missing).at("results").empty());
}
