#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <crow.h>
#include <nlohmann/json.hpp>

#include "api.h"
#include "core/search_engine.h"
#include "services/index_service.h"
#include "services/search_pipeline.h"
#include "test_support.h"

// The client below speaks just enough of RFC 6455 to check the real route: a
// handshake, masked text frames out and unmasked text frames in. The tests run
// on Linux only, like the container that builds them.

namespace {

using namespace std::chrono_literals;

using search::IndexService;
using search::SearchEngine;
using search::SearchPipeline;
using search::test_support::TempDir;

// One WebSocket frame as it appears on the wire.
struct Frame {
  int opcode = 0;
  std::string payload;
};

class WebSocketClient {
 public:
  WebSocketClient() = default;

  ~WebSocketClient() { Close(); }

  WebSocketClient(const WebSocketClient&) = delete;
  WebSocketClient& operator=(const WebSocketClient&) = delete;

  // Connects and performs the upgrade; retries until the server listens.
  bool ConnectWithRetry(int port, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (Connect(port)) {
        return true;
      }
      Close();
      std::this_thread::sleep_for(50ms);
    }
    return false;
  }

  bool SendText(std::string_view text) {
    std::string frame;
    frame.push_back(static_cast<char>(0x81));  // FIN + text
    const std::size_t size = text.size();
    if (size < 126) {
      frame.push_back(static_cast<char>(0x80U | size));  // masked
    } else if (size < 65536) {
      frame.push_back(static_cast<char>(0x80U | 126U));
      frame.push_back(static_cast<char>((size >> 8) & 0xFFU));
      frame.push_back(static_cast<char>(size & 0xFFU));
    } else {
      frame.push_back(static_cast<char>(0x80U | 127U));
      for (int shift = 56; shift >= 0; shift -= 8) {
        frame.push_back(static_cast<char>((size >> shift) & 0xFFU));
      }
    }
    const char mask[4] = {0x11, 0x22, 0x33, 0x44};
    frame.append(mask, sizeof(mask));
    for (std::size_t i = 0; i < size; ++i) {
      frame.push_back(static_cast<char>(text[i] ^ mask[i % 4]));
    }
    return WriteAll(frame);
  }

  // Reads the next text message, skipping control frames. Nothing on timeout.
  std::optional<nlohmann::json> ReadJson() {
    for (int attempts = 0; attempts < 8; ++attempts) {
      const std::optional<Frame> frame = ReadFrame();
      if (!frame) {
        return std::nullopt;
      }
      if (frame->opcode == 0x8) {  // close
        return std::nullopt;
      }
      if (frame->opcode != 0x1) {  // ping or pong
        continue;
      }
      nlohmann::json parsed =
          nlohmann::json::parse(frame->payload, nullptr, false);
      if (parsed.is_discarded()) {
        return std::nullopt;
      }
      return parsed;
    }
    return std::nullopt;
  }

 private:
  bool Connect(int port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
      return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) != 0) {
      return false;
    }
    timeval timeout{5, 0};
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    const std::string request =
        "GET /ws/search HTTP/1.1\r\n"
        "Host: 127.0.0.1:" + std::to_string(port) + "\r\n" +
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    if (!WriteAll(request)) {
      return false;
    }

    // The upgrade response ends with an empty line and no frames before the
    // first client message, so reading up to it is enough.
    std::string response;
    while (response.find("\r\n\r\n") == std::string::npos) {
      char byte = 0;
      const ssize_t read = ::recv(fd_, &byte, 1, 0);
      if (read <= 0 || response.size() > 4096) {
        return false;
      }
      response.push_back(byte);
    }
    return response.find("101 Switching Protocols") != std::string::npos;
  }

  void Close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  bool WriteAll(std::string_view data) const {
    std::size_t written = 0;
    while (written < data.size()) {
      const ssize_t sent =
          ::send(fd_, data.data() + written, data.size() - written, 0);
      if (sent <= 0) {
        return false;
      }
      written += static_cast<std::size_t>(sent);
    }
    return true;
  }

  bool ReadExact(char* data, std::size_t size) const {
    std::size_t read_total = 0;
    while (read_total < size) {
      const ssize_t read =
          ::recv(fd_, data + read_total, size - read_total, 0);
      if (read <= 0) {
        return false;
      }
      read_total += static_cast<std::size_t>(read);
    }
    return true;
  }

  std::optional<Frame> ReadFrame() const {
    char header[2];
    if (!ReadExact(header, sizeof(header))) {
      return std::nullopt;
    }
    const int opcode = static_cast<unsigned char>(header[0]) & 0x0F;
    std::uint64_t length = static_cast<unsigned char>(header[1]) & 0x7FU;
    if (length == 126) {
      char extended[2];
      if (!ReadExact(extended, sizeof(extended))) {
        return std::nullopt;
      }
      length = (static_cast<std::uint64_t>(
                    static_cast<unsigned char>(extended[0]))
                << 8) |
               static_cast<unsigned char>(extended[1]);
    } else if (length == 127) {
      char extended[8];
      if (!ReadExact(extended, sizeof(extended))) {
        return std::nullopt;
      }
      length = 0;
      for (char byte : extended) {
        length = (length << 8) | static_cast<unsigned char>(byte);
      }
    }
    // A server never masks its frames.
    std::string payload(static_cast<std::size_t>(length), '\0');
    if (!payload.empty() && !ReadExact(payload.data(), payload.size())) {
      return std::nullopt;
    }
    return Frame{opcode, std::move(payload)};
  }

  int fd_ = -1;
};

// Runs the real WebSocket route on a fixed port for the lifetime of the object.
class WebSocketServer {
 public:
  WebSocketServer(IndexService& service, const SearchPipeline& pipeline,
                  int port)
      : port_(port) {
    search::RegisterSearchSocketRoute(app_, service, pipeline);
    app_.loglevel(crow::LogLevel::Critical);
    app_.bindaddr("127.0.0.1").port(port_).multithreaded();
    worker_ = std::thread([this] { app_.run(); });
  }

  ~WebSocketServer() {
    app_.stop();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  WebSocketServer(const WebSocketServer&) = delete;
  WebSocketServer& operator=(const WebSocketServer&) = delete;

  int port() const { return port_; }

 private:
  const int port_;
  crow::SimpleApp app_;
  std::thread worker_;
};

search::QueryExpander FixedExpander(std::vector<std::string> terms) {
  return [terms = std::move(terms)](std::string_view) { return terms; };
}

}  // namespace

TEST(SearchSocketRoute, SendsAllStagesOverTheWire) {
  TempDir dir;
  dir.Write("a.txt", "whale");
  IndexService service(dir.path().string());
  ASSERT_TRUE(service.Build(2).has_value());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine, FixedExpander({"ocean"}));
  WebSocketServer server(service, pipeline, 18120);

  WebSocketClient client;
  ASSERT_TRUE(client.ConnectWithRetry(server.port(), 5s));
  ASSERT_TRUE(client.SendText(R"({"query": "whale"})"));

  const std::optional<nlohmann::json> expansion = client.ReadJson();
  ASSERT_TRUE(expansion.has_value());
  EXPECT_EQ("query_expansion", expansion->at("stage"));
  EXPECT_EQ("whale", expansion->at("data").at("original"));
  ASSERT_EQ(1U, expansion->at("data").at("expanded_terms").size());
  EXPECT_EQ("ocean", expansion->at("data").at("expanded_terms").at(0));

  const std::optional<nlohmann::json> search = client.ReadJson();
  ASSERT_TRUE(search.has_value());
  EXPECT_EQ("keyword_search", search->at("stage"));
  ASSERT_EQ(1U, search->at("data").at("results").size());
  EXPECT_EQ("a.txt", search->at("data").at("results").at(0).at("doc_id"));
  EXPECT_GE(search->at("data").at("time_ms").get<long long>(), 0);

  const std::optional<nlohmann::json> done = client.ReadJson();
  ASSERT_TRUE(done.has_value());
  EXPECT_EQ("done", done->at("stage"));
  EXPECT_FALSE(done->contains("data"));
}

TEST(SearchSocketRoute, KeepsTheConnectionOpenAfterAnError) {
  TempDir dir;
  dir.Write("a.txt", "zebra");
  IndexService service(dir.path().string());
  ASSERT_TRUE(service.Build(1).has_value());
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);
  WebSocketServer server(service, pipeline, 18121);

  WebSocketClient client;
  ASSERT_TRUE(client.ConnectWithRetry(server.port(), 5s));
  ASSERT_TRUE(client.SendText("not json"));

  const std::optional<nlohmann::json> error = client.ReadJson();
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ("error", error->at("stage"));
  EXPECT_EQ("invalid JSON body", error->at("data").at("message"));

  // The same connection serves the next query.
  ASSERT_TRUE(client.SendText(R"({"query": "zebra"})"));
  const std::optional<nlohmann::json> expansion = client.ReadJson();
  ASSERT_TRUE(expansion.has_value());
  EXPECT_EQ("query_expansion", expansion->at("stage"));
  const std::optional<nlohmann::json> search = client.ReadJson();
  ASSERT_TRUE(search.has_value());
  EXPECT_EQ("keyword_search", search->at("stage"));
  const std::optional<nlohmann::json> done = client.ReadJson();
  ASSERT_TRUE(done.has_value());
  EXPECT_EQ("done", done->at("stage"));
}
