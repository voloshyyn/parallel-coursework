#include <httplib.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

namespace {

void Usage(std::ostream& out) {
  out << "Usage: search [--server URL] [--json] [--] query\n"
         "Default server: SEARCH_URL or http://localhost:8080\n"
         "Exit codes: 0 success (including no matches), 1 request failure, "
         "2 invalid arguments.\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  const char* configured = std::getenv("SEARCH_URL");
  std::string server =
      configured && *configured ? configured : "http://localhost:8080";
  std::string query;
  bool json_output = false;
  bool positional = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (!positional && argument == "--help") {
      Usage(std::cout);
      return 0;
    }
    if (!positional && argument == "--") {
      positional = true;
    } else if (!positional && argument == "--json") {
      json_output = true;
    } else if (!positional && argument == "--server") {
      if (++i == argc) {
        Usage(std::cerr);
        return 2;
      }
      server = argv[i];
    } else if (!positional && argument.starts_with("--")) {
      std::cerr << "Unknown option: " << argument << '\n';
      return 2;
    } else {
      if (!query.empty()) {
        query += ' ';
      }
      query += argument;
    }
  }
  if (query.find_first_not_of(" \t\r\n") == std::string::npos) {
    Usage(std::cerr);
    return 2;
  }
  try {
    // HTTP only, just like the local coursework server. Reject path components
    // because httplib expects the constructor to contain an origin only.
    if (!server.starts_with("http://") || server.size() <= 7 ||
        server.find_first_of("/?#", 7) != std::string::npos) {
      std::cerr
          << "Server must be an HTTP origin, e.g. http://localhost:8080\n";
      return 2;
    }
    httplib::Client client(server);
    client.set_connection_timeout(std::chrono::seconds(3));
    client.set_max_timeout(std::chrono::seconds(30));
    const nlohmann::json request = {{"query", query}, {"mode", "keyword"}};
    const auto response =
        client.Post("/api/search", request.dump(), "application/json");
    if (!response) {
      std::cerr << "Cannot reach " << server << ": "
                << httplib::to_string(response.error()) << '\n';
      return 1;
    }
    if (response->status != 200) {
      std::cerr << "Search failed (HTTP " << response->status
                << "): " << response->body << '\n';
      return 1;
    }
    const auto body = nlohmann::json::parse(response->body);
    if (!body.is_object() || !body.contains("results") ||
        !body["results"].is_array() || !body.contains("time_ms") ||
        !body["time_ms"].is_number()) {
      throw std::runtime_error("invalid search response");
    }
    for (const auto& hit : body["results"]) {
      if (!hit.is_object() || !hit.contains("doc_id") ||
          !hit["doc_id"].is_string() || !hit.contains("score") ||
          !hit["score"].is_number() || !hit.contains("snippet") ||
          !hit["snippet"].is_string()) {
        throw std::runtime_error("invalid result entry");
      }
    }
    if (json_output) {
      std::cout << body.dump(2) << '\n';
      return 0;
    }
    std::cout << "Results: " << body["results"].size()
              << ", keyword search: " << body["time_ms"] << " ms\n";
    for (const auto& hit : body["results"]) {
      std::cout << '\n'
                << hit["doc_id"].get<std::string>() << " | score "
                << hit["score"] << '\n'
                << hit["snippet"].get<std::string>() << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Search error: " << error.what() << '\n';
    return 1;
  }
}
