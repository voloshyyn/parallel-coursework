#include "api.h"

#include <nlohmann/json.hpp>

namespace search {

std::string HealthJson(bool ollama_available) {
  const nlohmann::json body = {
      {"status", "ok"},
      {"ollama", ollama_available},
  };
  return body.dump();
}

void RegisterHealthRoute(crow::SimpleApp& app, const OllamaMonitor& monitor) {
  CROW_ROUTE(app, "/api/health")
      .methods(crow::HTTPMethod::GET)([&monitor] {
        crow::response response(200, HealthJson(monitor.IsAvailable()));
        response.set_header("Content-Type", "application/json");
        return response;
      });
}

}  // namespace search
