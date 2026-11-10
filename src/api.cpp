#include "api.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace search {
namespace {

using Json = nlohmann::json;

// Bounds from docs/api.md.
constexpr std::size_t kMinThreads = 1;
constexpr std::size_t kMaxThreads = 64;
constexpr std::size_t kDefaultThreads = 4;

crow::response JsonResponse(int status, const Json& body) {
  crow::response response(status, body.dump());
  response.set_header("Content-Type", "application/json");
  return response;
}

crow::response JsonError(int status, const std::string& message) {
  return JsonResponse(status, Json{{"status", "error"}, {"message", message}});
}

// Parses the optional `threads` field. Returns nothing and fills `error` when
// the value is present but not an integer in 1..64.
std::optional<std::size_t> ParseThreads(const Json& body, std::string& error) {
  if (!body.contains("threads")) {
    return kDefaultThreads;
  }
  const Json& value = body.at("threads");
  if (!value.is_number_integer()) {
    error = "threads must be in 1..64";
    return std::nullopt;
  }
  const auto threads = value.get<std::int64_t>();
  if (threads < static_cast<std::int64_t>(kMinThreads) ||
      threads > static_cast<std::int64_t>(kMaxThreads)) {
    error = "threads must be in 1..64";
    return std::nullopt;
  }
  return static_cast<std::size_t>(threads);
}

// Parses a request body that must be a JSON object. Returns nothing when the
// body is not valid JSON; `error` is filled in that case.
std::optional<Json> ParseObjectBody(const std::string& body, std::string& error) {
  const Json parsed = Json::parse(body, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    error = "invalid JSON body";
    return std::nullopt;
  }
  return parsed;
}

}  // namespace

std::string HealthJson(bool ollama_available) {
  const Json body = {
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

void RegisterSearchRoutes(crow::SimpleApp& app, IndexService& index_service,
                          const SearchEngine& engine) {
  CROW_ROUTE(app, "/api/index")
      .methods(crow::HTTPMethod::POST)([&index_service](
                                             const crow::request& request) {
        try {
          std::string error;
          const std::optional<Json> body =
              ParseObjectBody(request.body, error);
          if (!body) {
            return JsonError(400, error);
          }
          const std::optional<std::size_t> threads = ParseThreads(*body, error);
          if (!threads) {
            return JsonError(400, error);
          }
          const std::optional<BuildStats> stats = index_service.Build(*threads);
          if (!stats) {
            return JsonError(409, "index is building");
          }
          return JsonResponse(200,
                              Json{{"status", "ok"},
                                   {"documents_indexed",
                                    stats->documents_indexed},
                                   {"time_ms", stats->build_time_ms}});
        } catch (const std::exception& exception) {
          return JsonError(500, exception.what());
        }
      });

  CROW_ROUTE(app, "/api/search")
      .methods(crow::HTTPMethod::POST)([&index_service, &engine](
                                             const crow::request& request) {
        try {
          std::string error;
          const std::optional<Json> body =
              ParseObjectBody(request.body, error);
          if (!body) {
            return JsonError(400, error);
          }
          if (!body->contains("query") || !body->at("query").is_string() ||
              body->at("query").get<std::string>().empty()) {
            return JsonError(400, "query must be a non-empty string");
          }
          if (!body->contains("mode") || !body->at("mode").is_string() ||
              body->at("mode").get<std::string>() != "keyword") {
            return JsonError(400, "mode must be \"keyword\"");
          }

          if (index_service.State() == IndexState::kBuilding) {
            return JsonError(409, "index is building");
          }
          const std::shared_ptr<const InvertedIndex> index =
              index_service.Snapshot();
          if (!index) {
            return JsonError(409, "index not built");
          }

          const auto started = std::chrono::steady_clock::now();
          const std::vector<SearchResult> results =
              engine.Search(*index, body->at("query").get<std::string>());
          const auto elapsed =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - started)
                  .count();

          Json hits = Json::array();
          for (const SearchResult& result : results) {
            hits.push_back(Json{{"doc_id", result.doc_id},
                                {"score", result.score},
                                {"snippet", result.snippet}});
          }
          return JsonResponse(200,
                              Json{{"results", std::move(hits)},
                                   {"time_ms", elapsed}});
        } catch (const std::exception& exception) {
          return JsonError(500, exception.what());
        }
      });

  CROW_ROUTE(app, "/api/stats")
      .methods(crow::HTTPMethod::GET)([&index_service] {
        const std::shared_ptr<const InvertedIndex> index =
            index_service.Snapshot();
        const BuildStats stats = index_service.Stats();
        return JsonResponse(
            200, Json{{"documents", index ? index->DocumentCount() : 0},
                      {"index_build_time_ms", stats.build_time_ms},
                      {"threads_used", stats.threads_used}});
      });
}

}  // namespace search
