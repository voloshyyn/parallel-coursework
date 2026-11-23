#include "web_ui.h"

#include <crow.h>
#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

// Runs the real UI routes on a fixed port for the lifetime of the object.
class UiServer {
 public:
  explicit UiServer(int port) : port_(port) {
    search::RegisterUiRoutes(app_);
    app_.loglevel(crow::LogLevel::Critical);
    app_.bindaddr("127.0.0.1").port(port_).multithreaded();
    worker_ = std::thread([this] { app_.run(); });
  }

  ~UiServer() {
    app_.stop();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  UiServer(const UiServer&) = delete;
  UiServer& operator=(const UiServer&) = delete;

  bool WaitUntilReady() {
    httplib::Client client("127.0.0.1", port_);
    client.set_connection_timeout(200ms);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (client.Get("/")) {
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
  client.set_read_timeout(5s);
  return client;
}

}  // namespace

TEST(WebUi, ServesThePageThatLoadsTheApi) {
  UiServer server(18130);
  ASSERT_TRUE(server.WaitUntilReady());
  httplib::Client client = MakeClient(server.port());

  const auto page = client.Get("/");
  ASSERT_TRUE(page);
  EXPECT_EQ(200, page->status);
  EXPECT_NE(std::string::npos,
            page->get_header_value("Content-Type").find("text/html"));
  EXPECT_EQ("no-cache", page->get_header_value("Cache-Control"));

  // Vue mounts here, and every asset comes from the same binary.
  EXPECT_NE(std::string::npos, page->body.find("id=\"app\""));
  EXPECT_NE(std::string::npos, page->body.find("v-cloak"));
  EXPECT_NE(std::string::npos, page->body.find("/vue.global.prod.js"));
  EXPECT_NE(std::string::npos, page->body.find("/search_client.js"));
  EXPECT_NE(std::string::npos, page->body.find("/app.js"));
  EXPECT_NE(std::string::npos, page->body.find("/style.css"));
  // Regression: a CDN script made an offline demo render a blank page.
  EXPECT_EQ(std::string::npos, page->body.find("https://"));
  EXPECT_EQ(std::string::npos, page->body.find("unpkg"));
  EXPECT_EQ(std::string::npos, page->body.find("cdn"));
  // The snippet and the terms are rendered with escaping interpolation.
  EXPECT_EQ(std::string::npos, page->body.find("v-html"));
}

TEST(WebUi, ServesTheSearchClient) {
  UiServer server(18131);
  ASSERT_TRUE(server.WaitUntilReady());
  httplib::Client client = MakeClient(server.port());

  const auto script = client.Get("/search_client.js");
  ASSERT_TRUE(script);
  EXPECT_EQ(200, script->status);
  EXPECT_NE(std::string::npos,
            script->get_header_value("Content-Type").find("javascript"));
  // The state machine speaks the documented protocol and never injects HTML.
  EXPECT_NE(std::string::npos, script->body.find("/ws/search"));
  EXPECT_NE(std::string::npos, script->body.find("WebSocket"));
  EXPECT_NE(std::string::npos, script->body.find("expanded_terms"));
  EXPECT_NE(std::string::npos, script->body.find("/api/ready"));
  EXPECT_NE(std::string::npos, script->body.find("validateMessage"));
  EXPECT_EQ(std::string::npos, script->body.find("innerHTML"));
}

TEST(WebUi, ServesTheAppWrapperWithoutAnExternalNetwork) {
  UiServer server(18134);
  ASSERT_TRUE(server.WaitUntilReady());
  httplib::Client client = MakeClient(server.port());

  const auto script = client.Get("/app.js");
  ASSERT_TRUE(script);
  EXPECT_EQ(200, script->status);
  EXPECT_NE(std::string::npos, script->body.find("createSearchClient"));
  // The no-Vue fallback must be visible: it removes v-cloak instead of writing
  // into a hidden element.
  EXPECT_NE(std::string::npos, script->body.find("removeAttribute('v-cloak')"));
  EXPECT_EQ(std::string::npos, script->body.find("innerHTML"));
  EXPECT_EQ(std::string::npos, script->body.find("https://"));
}

TEST(WebUi, ServesPinnedVueFromTheBinary) {
  UiServer server(18135);
  ASSERT_TRUE(server.WaitUntilReady());
  httplib::Client client = MakeClient(server.port());

  const auto vue = client.Get("/vue.global.prod.js");
  ASSERT_TRUE(vue);
  EXPECT_EQ(200, vue->status);
  EXPECT_NE(std::string::npos,
            vue->get_header_value("Content-Type").find("javascript"));
  EXPECT_GT(vue->body.size(), 100000U);
  EXPECT_NE(std::string::npos, vue->body.find("Vue"));
}

TEST(WebUi, ServesTheStyles) {
  UiServer server(18132);
  ASSERT_TRUE(server.WaitUntilReady());
  httplib::Client client = MakeClient(server.port());

  const auto styles = client.Get("/style.css");
  ASSERT_TRUE(styles);
  EXPECT_EQ(200, styles->status);
  EXPECT_NE(std::string::npos,
            styles->get_header_value("Content-Type").find("text/css"));
  EXPECT_NE(std::string::npos, styles->body.find("[v-cloak]"));
}

TEST(WebUi, LeavesUnknownPathsToTheApi) {
  UiServer server(18133);
  ASSERT_TRUE(server.WaitUntilReady());
  httplib::Client client = MakeClient(server.port());

  const auto missing = client.Get("/missing");
  ASSERT_TRUE(missing);
  EXPECT_EQ(404, missing->status);
}
