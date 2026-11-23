#include <gtest/gtest.h>
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "api.h"
#include "integrations/ollama_monitor.h"
#include "services/index_service.h"
#include "test_support.h"

namespace {

using namespace std::chrono_literals;

using search::IndexService;
using search::test_support::BuildGate;
using search::test_support::TempDir;

// Runs the real health route on a fixed port for the lifetime of the object.
class HealthServer {
 public:
  HealthServer(search::OllamaMonitor& monitor, int port,
               const IndexService* index_service = nullptr,
               const std::string& startup_status_file = {})
      : port_(port) {
    search::RegisterHealthRoute(app_, monitor, index_service,
                                startup_status_file);
    app_.loglevel(crow::LogLevel::Critical);
    app_.bindaddr("127.0.0.1").port(port_).multithreaded();
    worker_ = std::thread([this] { app_.run(); });
  }

  ~HealthServer() {
    app_.stop();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  HealthServer(const HealthServer&) = delete;
  HealthServer& operator=(const HealthServer&) = delete;

  // Waits until the port accepts connections.
  bool WaitUntilReady() {
    httplib::Client client("127.0.0.1", port_);
    client.set_connection_timeout(100ms);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (client.Get("/api/health")) {
        return true;
      }
      std::this_thread::sleep_for(20ms);
    }
    return false;
  }

  int port() const { return port_; }

 private:
  int port_;
  crow::SimpleApp app_;
  std::thread worker_;
};

httplib::Client MakeClient(int port) {
  httplib::Client client("127.0.0.1", port);
  client.set_connection_timeout(1s);
  client.set_read_timeout(1s);
  return client;
}

}  // namespace

// A synchronous probe would block the handler for a whole second, so a fast
// answer proves the endpoint reads a cached value.
TEST(HealthEndpoint, AnswersFasterThan200MsWhileProbeIsSlow) {
  search::OllamaMonitor monitor(
      [] {
        std::this_thread::sleep_for(1s);
        return false;
      },
      5s);
  monitor.Start();

  HealthServer server(monitor, 18081);
  ASSERT_TRUE(server.WaitUntilReady());

  httplib::Client client = MakeClient(server.port());
  const auto started = std::chrono::steady_clock::now();
  const auto result = client.Get("/api/health");
  const auto elapsed = std::chrono::steady_clock::now() - started;

  ASSERT_TRUE(result) << "health endpoint did not answer";
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  EXPECT_LT(elapsed, 200ms) << "health took " << elapsed_ms << " ms";
  EXPECT_EQ(200, result->status);
  EXPECT_NE(std::string::npos, result->body.find("\"status\":\"ok\""));
  EXPECT_NE(std::string::npos, result->body.find("\"ollama\":false"));
}

TEST(HealthEndpoint, Returns200AndCachedAvailability) {
  search::OllamaMonitor monitor([] { return true; }, 100ms);
  monitor.Start();

  HealthServer server(monitor, 18082);
  ASSERT_TRUE(server.WaitUntilReady());

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!monitor.IsAvailable() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }

  httplib::Client client = MakeClient(server.port());
  const auto result = client.Get("/api/health");

  ASSERT_TRUE(result) << "health endpoint did not answer";
  EXPECT_EQ(200, result->status);
  EXPECT_NE(std::string::npos, result->body.find("\"ollama\":true"));
  EXPECT_EQ(2U, nlohmann::json::parse(result->body).size());
}

// Our extension for automatic startup indexing: HTTP answers 200 immediately,
// but the client can see whether search is usable yet.
TEST(HealthEndpoint, ReportsIndexStateAndReadiness) {
  search::OllamaMonitor monitor([] { return false; }, 5s);
  TempDir dir;
  dir.Write("a.txt", "alpha");
  IndexService service(dir.path().string());
  HealthServer server(monitor, 18083, &service);
  ASSERT_TRUE(server.WaitUntilReady());
  httplib::Client client = MakeClient(server.port());

  const auto empty = client.Get("/api/ready");
  ASSERT_TRUE(empty);
  EXPECT_EQ(200, empty->status);
  EXPECT_NE(std::string::npos, empty->body.find("\"index_state\":\"empty\""));
  EXPECT_NE(std::string::npos, empty->body.find("\"search_ready\":false"));

  ASSERT_TRUE(service.Build(1).has_value());
  const auto ready = client.Get("/api/ready");
  ASSERT_TRUE(ready);
  EXPECT_EQ(200, ready->status);
  EXPECT_NE(std::string::npos, ready->body.find("\"index_state\":\"ready\""));
  EXPECT_NE(std::string::npos, ready->body.find("\"search_ready\":true"));
  EXPECT_NE(std::string::npos, ready->body.find("\"index_error\":\"\""));
}

TEST(HealthEndpoint, ReportsABuildingIndex) {
  search::OllamaMonitor monitor([] { return false; }, 5s);
  TempDir dir;
  dir.Write("a.txt", "alpha");
  BuildGate gate("whale");
  gate.Hold("whale");
  IndexService service(
      dir.path().string(),
      [&gate](const std::filesystem::path& path) { return gate.Read(path); });
  HealthServer server(monitor, 18084, &service);
  ASSERT_TRUE(server.WaitUntilReady());
  httplib::Client client = MakeClient(server.port());

  std::thread builder([&service] { service.Build(1); });
  gate.WaitUntilReading();

  const auto building = client.Get("/api/ready");
  gate.Release();
  builder.join();

  ASSERT_TRUE(building);
  EXPECT_EQ(200, building->status);
  EXPECT_NE(std::string::npos,
            building->body.find("\"index_state\":\"building\""));
  EXPECT_NE(std::string::npos, building->body.find("\"search_ready\":false"));
}

TEST(HealthEndpoint, ExposesPreparationFailureWithoutChangingHealthContract) {
  search::OllamaMonitor monitor([] { return false; }, 5s);
  TempDir dir;
  dir.Write("startup.json",
            R"({"state":"error","error":"dataset unavailable"})");
  IndexService service(dir.path().string());
  HealthServer server(monitor, 18085, &service,
                      (dir.path() / "startup.json").string());
  ASSERT_TRUE(server.WaitUntilReady());
  httplib::Client client = MakeClient(server.port());
  const auto ready = client.Get("/api/ready");
  ASSERT_TRUE(ready);
  const auto body = nlohmann::json::parse(ready->body);
  EXPECT_EQ("error", body.at("startup_state"));
  EXPECT_EQ("dataset unavailable", body.at("startup_error"));
  EXPECT_FALSE(body.at("search_ready").get<bool>());
  const auto health = client.Get("/api/health");
  ASSERT_TRUE(health);
  EXPECT_EQ(2U, nlohmann::json::parse(health->body).size());
}

TEST(OllamaMonitor, PublishesProbeResult) {
  std::atomic<int> calls{0};
  search::OllamaMonitor monitor(
      [&calls] {
        calls.fetch_add(1);
        return true;
      },
      50ms);
  EXPECT_FALSE(monitor.IsAvailable());

  monitor.Start();
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!monitor.IsAvailable() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  monitor.Stop();

  EXPECT_TRUE(monitor.IsAvailable());
  EXPECT_GE(calls.load(), 1);
}
