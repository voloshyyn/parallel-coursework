#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace search {

// Minimal client for the Ollama HTTP API.
class OllamaClient {
 public:
  // `timeout` bounds the health probe, `generation_timeout` bounds query
  // expansion: a small model needs a few seconds on CPU, while the health
  // check must stay short.
  explicit OllamaClient(
      std::string base_url,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(500),
      std::chrono::milliseconds generation_timeout =
          std::chrono::milliseconds(5000));

  // Returns true when the Ollama server answers GET /api/tags with 200.
  bool IsAvailable() const;

  // Asks `model` to expand `query` into synonyms and related keywords and
  // returns their tokens: lowercased, unique, without the query's own terms,
  // at most eight. Never throws: a connection error, a timeout, a non-200
  // answer or a malformed body yield an empty list, so the caller can fall back
  // to the original query (docs/api.md).
  std::vector<std::string> ExpandQuery(std::string_view query,
                                       std::string_view model) const;

 private:
  std::string base_url_;
  std::chrono::milliseconds timeout_;
  std::chrono::milliseconds generation_timeout_;
};

}  // namespace search
