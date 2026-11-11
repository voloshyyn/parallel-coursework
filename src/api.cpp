#include "api.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
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

// One hit shape for REST and WebSocket, so both always carry doc_id, score and
// snippet and nothing else.
Json HitsJson(const std::vector<SearchResult>& results) {
  Json hits = Json::array();
  for (const SearchResult& result : results) {
    hits.push_back(Json{{"doc_id", result.doc_id},
                        {"score", result.score},
                        {"snippet", result.snippet}});
  }
  return hits;
}

void SendStage(crow::websocket::connection& connection, const Json& message) {
  connection.send_text(message.dump());
}

// Our own convention, not part of the syllabus contract: an unusable request
// produces exactly one error stage, and the connection stays open.
void SendSocketError(crow::websocket::connection& connection,
                     const std::string& message) {
  SendStage(connection, Json{{"stage", "error"},
                             {"data", Json{{"message", message}}}});
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
                          const SearchPipeline& pipeline) {
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
      .methods(crow::HTTPMethod::POST)([&index_service, &pipeline](
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
          const PublishedIndex snapshot = index_service.Snapshot();
          if (!snapshot.index) {
            return JsonError(409, "index not built");
          }

          const std::string query = body->at("query").get<std::string>();
          // Expansion runs before the search and never fails the request: a
          // missing Ollama leaves the original query in place.
          const SearchOutcome outcome = pipeline.Run(*snapshot.index, query);

          return JsonResponse(200,
                              Json{{"results", HitsJson(outcome.results)},
                                   {"time_ms", outcome.search_time_ms}});
        } catch (const std::exception& exception) {
          return JsonError(500, exception.what());
        }
      });

  CROW_ROUTE(app, "/api/stats")
      .methods(crow::HTTPMethod::GET)([&index_service] {
        // One snapshot: documents, build time and threads always describe the
        // same successful build.
        const PublishedIndex snapshot = index_service.Snapshot();
        return JsonResponse(
            200,
            Json{{"documents",
                  snapshot.index ? snapshot.index->DocumentCount() : 0},
                 {"index_build_time_ms", snapshot.stats.build_time_ms},
                 {"threads_used", snapshot.stats.threads_used}});
      });
}

void HandleSearchSocketMessage(crow::websocket::connection& connection,
                               IndexService& index_service,
                               const SearchPipeline& pipeline,
                               const std::string& message) {
  try {
    std::string error;
    const std::optional<Json> body = ParseObjectBody(message, error);
    if (!body) {
      SendSocketError(connection, error);
      return;
    }
    if (!body->contains("query") || !body->at("query").is_string() ||
        body->at("query").get<std::string>().empty()) {
      SendSocketError(connection, "query must be a non-empty string");
      return;
    }

    // The same index rules as POST /api/search: no search while a build runs
    // and none before the first successful build.
    if (index_service.State() == IndexState::kBuilding) {
      SendSocketError(connection, "index is building");
      return;
    }
    const PublishedIndex snapshot = index_service.Snapshot();
    if (!snapshot.index) {
      SendSocketError(connection, "index not built");
      return;
    }

    const SearchOutcome outcome =
        pipeline.Run(*snapshot.index, body->at("query").get<std::string>());

    // One message per stage, in the order docs/api.md fixes. REST hides the
    // expanded terms; this is where they reach the client.
    Json terms = Json::array();
    for (const std::string& term : outcome.expanded_terms) {
      terms.push_back(term);
    }
    SendStage(connection,
              Json{{"stage", "query_expansion"},
                   {"data", Json{{"original", outcome.original_query},
                                 {"expanded_terms", std::move(terms)}}}});
    SendStage(connection,
              Json{{"stage", "keyword_search"},
                   {"data", Json{{"results", HitsJson(outcome.results)},
                                 {"time_ms", outcome.search_time_ms}}}});
    SendStage(connection, Json{{"stage", "done"}});
  } catch (const std::exception& exception) {
    SendSocketError(connection, exception.what());
  }
}

void RegisterSearchSocketRoute(crow::SimpleApp& app,
                               IndexService& index_service,
                               const SearchPipeline& pipeline) {
  CROW_WEBSOCKET_ROUTE(app, "/ws/search")
      .onmessage([&index_service, &pipeline](
                     crow::websocket::connection& connection,
                     const std::string& message, bool /*is_binary*/) {
        HandleSearchSocketMessage(connection, index_service, pipeline, message);
      });
}

}  // namespace search
