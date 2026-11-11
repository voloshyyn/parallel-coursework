#include "integrations/ollama_client.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

using search::OllamaClient;

// Serves canned answers on POST /api/generate and answers GET /api/tags, so a
// test can wait until the port is ready without racing the listener thread.
class MockOllama {
 public:
  using Handler =
      std::function<void(const httplib::Request&, httplib::Response&)>;

  explicit MockOllama(int port, Handler handler = {})
      : port_(port), handler_(std::move(handler)) {
    server_.Get("/api/tags",
                [](const httplib::Request&, httplib::Response& response) {
                  response.status = 200;
                  response.set_content(R"({"models":[]})", "application/json");
                });
    server_.Post("/api/generate",
                 [this](const httplib::Request& request,
                        httplib::Response& response) {
                   {
                     std::lock_guard<std::mutex> lock(mutex_);
                     last_request_ = request.body;
                     ++calls_;
                   }
                   if (handler_) {
                     handler_(request, response);
                     return;
                   }
                   response.status = 200;
                   response.set_content(R"({"response":""})",
                                        "application/json");
                 });
    bound_ = server_.bind_to_port("127.0.0.1", port_);
    if (bound_) {
      worker_ = std::thread([this] { server_.listen_after_bind(); });
    }
  }

  ~MockOllama() {
    server_.stop();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  MockOllama(const MockOllama&) = delete;
  MockOllama& operator=(const MockOllama&) = delete;

  // Waits until the listener accepts connections, so the client never sees a
  // half-started server.
  bool WaitUntilReady() {
    httplib::Client client("127.0.0.1", port_);
    client.set_connection_timeout(100ms);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (client.Get("/api/tags")) {
        return true;
      }
      std::this_thread::sleep_for(5ms);
    }
    return false;
  }

  int port() const { return port_; }

  std::string LastRequest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_request_;
  }

  int Calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return calls_;
  }

 private:
  const int port_;
  Handler handler_;
  httplib::Server server_;
  std::thread worker_;
  bool bound_ = false;

  mutable std::mutex mutex_;
  std::string last_request_;
  int calls_ = 0;
};

// Responds with a fixed status and body after an optional delay.
MockOllama::Handler Reply(int status, std::string body,
                          std::chrono::milliseconds delay = 0ms) {
  return [status, body = std::move(body), delay](const httplib::Request&,
                                                 httplib::Response& response) {
    if (delay > 0ms) {
      std::this_thread::sleep_for(delay);
    }
    response.status = status;
    response.set_content(body, "application/json");
  };
}

std::string PortUrl(int port) {
  return "http://127.0.0.1:" + std::to_string(port);
}

}  // namespace

TEST(OllamaClient, ExpandsQueryWithGeneratedTerms) {
  MockOllama server(18110, Reply(200, R"({"response":"whale, ocean, sea"})"));
  ASSERT_TRUE(server.WaitUntilReady());
  const OllamaClient client(PortUrl(server.port()), 500ms, 2s);

  const std::vector<std::string> terms =
      client.ExpandQuery("big fish", "qwen2.5:1.5b");

  ASSERT_EQ(3U, terms.size());
  EXPECT_EQ("whale", terms[0]);
  EXPECT_EQ("ocean", terms[1]);
  EXPECT_EQ("sea", terms[2]);

  // The request tells Ollama which model to use and asks for one answer.
  const nlohmann::json request = nlohmann::json::parse(server.LastRequest());
  EXPECT_EQ("qwen2.5:1.5b", request.at("model"));
  EXPECT_EQ(false, request.at("stream"));
  EXPECT_NE(std::string::npos,
            request.at("prompt").get<std::string>().find("big fish"));
}

TEST(OllamaClient, DropsDuplicatesAndTermsAlreadyInTheQuery) {
  MockOllama server(18111,
                    Reply(200, R"({"response":"Whale, whale, BIG, ocean"})"));
  ASSERT_TRUE(server.WaitUntilReady());
  const OllamaClient client(PortUrl(server.port()), 500ms, 2s);

  const std::vector<std::string> terms =
      client.ExpandQuery("big fish", "model");

  // Folding and deduplication follow the tokenizer used by the index.
  ASSERT_EQ(2U, terms.size());
  EXPECT_EQ("whale", terms[0]);
  EXPECT_EQ("ocean", terms[1]);
}

TEST(OllamaClient, LimitsTheNumberOfExpandedTerms) {
  MockOllama server(
      18112, Reply(200, R"({"response":"t1 t2 t3 t4 t5 t6 t7 t8 t9 t10"})"));
  ASSERT_TRUE(server.WaitUntilReady());
  const OllamaClient client(PortUrl(server.port()), 500ms, 2s);

  const std::vector<std::string> terms = client.ExpandQuery("query", "model");

  ASSERT_EQ(8U, terms.size());
  EXPECT_EQ("t1", terms.front());
  EXPECT_EQ("t8", terms.back());
}

TEST(OllamaClient, ReturnsNothingOnServerError) {
  MockOllama server(18113, Reply(500, R"({"error":"boom"})"));
  ASSERT_TRUE(server.WaitUntilReady());
  const OllamaClient client(PortUrl(server.port()), 500ms, 2s);

  EXPECT_TRUE(client.ExpandQuery("big fish", "model").empty());
}

TEST(OllamaClient, ReturnsNothingOnMalformedBody) {
  MockOllama server(18114, Reply(200, "not json"));
  ASSERT_TRUE(server.WaitUntilReady());
  const OllamaClient client(PortUrl(server.port()), 500ms, 2s);

  EXPECT_TRUE(client.ExpandQuery("big fish", "model").empty());
}

TEST(OllamaClient, ReturnsNothingWhenTheServerIsUnreachable) {
  int closed_port = 0;
  {
    MockOllama server(18115, Reply(200, R"({"response":"whale"})"));
    ASSERT_TRUE(server.WaitUntilReady());
    closed_port = server.port();
  }
  // The mock is gone: the port refuses connections, like a stopped Ollama.
  const OllamaClient client(PortUrl(closed_port), 200ms, 200ms);

  EXPECT_TRUE(client.ExpandQuery("big fish", "model").empty());
}

TEST(OllamaClient, StopsWaitingWhenGenerationTimesOut) {
  MockOllama server(18116, Reply(200, R"({"response":"whale"})", 500ms));
  ASSERT_TRUE(server.WaitUntilReady());
  // The generation timeout is much shorter than the server delay, so the call
  // must return early and report no expansion.
  const OllamaClient client(PortUrl(server.port()), 500ms, 50ms);

  const auto started = std::chrono::steady_clock::now();
  const std::vector<std::string> terms = client.ExpandQuery("big fish", "m");
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_TRUE(terms.empty());
  EXPECT_LT(elapsed, 400ms);
}

TEST(OllamaClient, ProbesAvailabilityWithTheTagsEndpoint) {
  MockOllama server(18117, Reply(200, R"({"response":"whale"})"));
  ASSERT_TRUE(server.WaitUntilReady());

  const OllamaClient available(PortUrl(server.port()), 500ms, 2s);
  EXPECT_TRUE(available.IsAvailable());

  const OllamaClient unavailable("http://127.0.0.1:1", 200ms, 200ms);
  EXPECT_FALSE(unavailable.IsAvailable());
}
