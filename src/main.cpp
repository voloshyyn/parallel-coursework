#include <crow.h>

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>

#include "config.h"
#include "integrations/ollama_client.h"

namespace {

std::string HealthJson(const search::OllamaClient& ollama) {
  const nlohmann::json body = {
      {"status", "ok"},
      {"ollama", ollama.IsAvailable()},
  };
  return body.dump();
}

}  // namespace

int main() {
  const search::Config config = search::Config::FromEnvironment();
  const search::OllamaClient ollama(config.ollama_url);

  crow::SimpleApp app;
  CROW_ROUTE(app, "/api/health")
      .methods(crow::HTTPMethod::GET)([&ollama] {
        crow::response response(200, HealthJson(ollama));
        response.set_header("Content-Type", "application/json");
        return response;
      });

  app.loglevel(crow::LogLevel::Warning);
  std::cout << "search_server listening on 0.0.0.0:" << config.port
            << std::endl;
  app.bindaddr("0.0.0.0").port(config.port).multithreaded().run();
  return 0;
}
