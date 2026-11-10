#include <gtest/gtest.h>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "api.h"
#include "integrations/ollama_monitor.h"

namespace {

using namespace std::chrono_literals;

// Runs the real health route on a fixed port for the lifetime of the object.
class HealthServer {
 public:
  HealthServer(search::OllamaMonitor& monitor, int port) : port_(port) {
    search::RegisterHealthRoute(app_, monitor);
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
