#include "integrations/ollama_client.h"

#include <gtest/gtest.h>
#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <utility>
#include <vector>

TEST(OllamaModelHealth, RequiresTheSelectedModel) {
  httplib::Server server;
  server.Get("/api/tags", [](const auto&, auto& response) {
    response.set_content(R"({"models":[{"name":"qwen2.5:1.5b"}]})",
                         "application/json");
  });
  const int port = server.bind_to_any_port("127.0.0.1");
  ASSERT_GT(port, 0);
  std::thread listener([&server] { server.listen_after_bind(); });
  const search::OllamaClient client("http://127.0.0.1:" + std::to_string(port));
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!client.IsAvailable() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(client.IsAvailable("qwen2.5:1.5b"));
  EXPECT_FALSE(client.IsAvailable("missing:1b"));
  server.stop();
  listener.join();
}

namespace {

using namespace std::chrono_literals;

using search::OllamaClient;

// Serves canned answers on POST /api/generate and answers GET /api/tags, so a
// test can wait until the port is ready without racing the listener thread.
class MockOllama {
 public:
  using Handler =
      std::function<void(const httplib::Request&, httplib::Response&)>;

  explicit MockOllama(int port, Handler handler = {},
                      std::string tags = R"({"models":[]})")
      : port_(port), handler_(std::move(handler)) {
    server_.Get("/api/tags",
                [tags = std::move(tags)](const httplib::Request&,
                                         httplib::Response& response) {
                  response.status = 200;
                  response.set_content(tags, "application/json");
                });
    server_.Post("/api/generate", [this](const httplib::Request& request,
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
      response.set_content(R"({"response":""})", "application/json");
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

// Streams a JSON answer in small pieces with a pause between them. A per-read
// timeout never fires because every wait is short; only a total-request
// deadline can stop the call.
MockOllama::Handler SlowChunkedReply(std::chrono::milliseconds pause,
                                     int chunks) {
  return [pause, chunks](const httplib::Request&, httplib::Response& response) {
    response.status = 200;
    response.set_chunked_content_provider(
        "application/json", [pause, chunks, sent = 0](
                                std::size_t, httplib::DataSink& sink) mutable {
          if (sent == 0) {
            const std::string head = R"({"response":")";
            sink.write(head.data(), head.size());
          }
          if (sent < chunks) {
            std::this_thread::sleep_for(pause);
            const std::string piece = "whale ";
            sink.write(piece.data(), piece.size());
            ++sent;
            return true;
          }
          const std::string tail = R"("})";
          sink.write(tail.data(), tail.size());
          sink.done();
          return true;
        });
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

// Regression: "1. whale" used to tokenize as "1", "whale", so the numbers ate
// the eight-term budget and pushed real terms out.
TEST(OllamaClient, IgnoresNumberedListMarkers) {
  MockOllama server(
      18141,
      Reply(
          200,
          R"({"response":"1. whale\n2. ocean\n3. sea\n4. marine\n5. fish"})"));
  ASSERT_TRUE(server.WaitUntilReady());
  const OllamaClient client(PortUrl(server.port()), 500ms, 2s);

  const std::vector<std::string> terms = client.ExpandQuery("big", "model");

  ASSERT_EQ(5U, terms.size());
  EXPECT_EQ("whale", terms[0]);
  EXPECT_EQ("ocean", terms[1]);
  EXPECT_EQ("sea", terms[2]);
  EXPECT_EQ("marine", terms[3]);
  EXPECT_EQ("fish", terms[4]);
}

TEST(OllamaClient, IgnoresInlineNumberedMarkers) {
  MockOllama server(
      18143, Reply(200, R"({"response":"1. whale, 2. sea, 3. marine"})"));
  ASSERT_TRUE(server.WaitUntilReady());
  const OllamaClient client(PortUrl(server.port()), 500ms, 2s);

  const std::vector<std::string> terms = client.ExpandQuery("big", "model");

  ASSERT_EQ(3U, terms.size());
  EXPECT_EQ("whale", terms[0]);
  EXPECT_EQ("sea", terms[1]);
  EXPECT_EQ("marine", terms[2]);
}

// The marker stripper must not eat content: a technical token such as C++20
// and a four-digit year are data, not list numbers.
TEST(OllamaClient, KeepsMeaningfulNumbersInTheAnswer) {
  MockOllama server(18142, Reply(200, R"({"response":"C++20, 2020, whale"})"));
  ASSERT_TRUE(server.WaitUntilReady());
  const OllamaClient client(PortUrl(server.port()), 500ms, 2s);

  const std::vector<std::string> terms = client.ExpandQuery("fish", "model");

  EXPECT_NE(terms.end(), std::find(terms.begin(), terms.end(), "c"));
  EXPECT_NE(terms.end(), std::find(terms.begin(), terms.end(), "20"));
  EXPECT_NE(terms.end(), std::find(terms.begin(), terms.end(), "2020"));
  EXPECT_NE(terms.end(), std::find(terms.begin(), terms.end(), "whale"));
}

// Regression: read/write timeouts are per operation, so a model that keeps
// sending tokens never hit them. The whole generation must respect one
// deadline.
TEST(OllamaClient, StopsAVerySlowStreamAtTheGenerationDeadline) {
  MockOllama server(18140, SlowChunkedReply(30ms, 40));  // ~1.2 s unabridged
  ASSERT_TRUE(server.WaitUntilReady());
  const OllamaClient client(PortUrl(server.port()), 500ms, 50ms);

  const auto started = std::chrono::steady_clock::now();
  const std::vector<std::string> terms = client.ExpandQuery("big", "model");
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_TRUE(terms.empty());
  EXPECT_LT(elapsed, 400ms);
}
