#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <filesystem>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

#include <crow.h>
#include <nlohmann/json.hpp>

#include "api.h"
#include "core/search_engine.h"
#include "services/index_service.h"
#include "test_support.h"

namespace {

using namespace std::chrono_literals;

using search::IndexService;
using search::IndexState;
using search::SearchEngine;
using search::test_support::TempDir;

// Runs the REST search routes on a fixed port for the lifetime of the object.
class ApiServer {
 public:
  ApiServer(IndexService& service, const SearchEngine& engine, int port)
      : port_(port) {
    search::RegisterSearchRoutes(app_, service, engine);
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

TEST(SearchApi, RejectsInvalidThreads) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  IndexService service(dir.path().string());
  const SearchEngine engine(dir.path().string());
  ApiServer server(service, engine, 18090);
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
  ApiServer server(service, engine, 18091);
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
  ApiServer server(service, engine, 18092);
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
  ApiServer server(service, engine, 18093);
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
  ApiServer server(service, engine, 18094);
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
  ApiServer server(service, engine, 18095);
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

  // The blocking reader holds the build open, so the 409 answer is observed
  // deterministically instead of by racing a fast build.
  std::binary_semaphore gate{0};
  IndexService service(dir.path().string(),
                       [&gate](const std::filesystem::path&) {
                         gate.acquire();
                         gate.release();
                         return std::string("whale whale");
                       });
  const SearchEngine engine(dir.path().string());
  ApiServer server(service, engine, 18096);
  ASSERT_TRUE(server.WaitUntilReady());

  int build_status = 0;
  std::thread builder([&service, &build_status] {
    httplib::Client client("127.0.0.1", 18096);
    client.set_read_timeout(30s);
    const auto result = client.Post("/api/index", R"({"threads": 1})",
                                    "application/json");
    build_status = result ? result->status : -1;
  });

  const bool observed_building = WaitUntilBuilding(service, 5s);

  httplib::Client client = MakeClient(server.port());
  const auto during_build = client.Post(
      "/api/search", R"({"query": "whale", "mode": "keyword"})",
      "application/json");

  gate.release();
  builder.join();

  ASSERT_TRUE(observed_building);
  ASSERT_TRUE(during_build);
  EXPECT_EQ(409, during_build->status);
  EXPECT_NE(std::string::npos,
            during_build->body.find("\"message\":\"index is building\""));
  EXPECT_EQ(200, build_status);

  const auto after_build = client.Post(
      "/api/search", R"({"query": "whale", "mode": "keyword"})",
      "application/json");
  ASSERT_TRUE(after_build);
  EXPECT_EQ(200, after_build->status);
  EXPECT_EQ(2U, ParseBody(after_build).at("results").size());
}
