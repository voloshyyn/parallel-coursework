#pragma once

#include <chrono>
#include <string>

namespace search {

// Minimal client for the Ollama HTTP API.
class OllamaClient {
 public:
  explicit OllamaClient(
      std::string base_url,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(500));

  // Returns true when the Ollama server answers GET /api/tags with 200.
  bool IsAvailable() const;

 private:
  std::string base_url_;
  std::chrono::milliseconds timeout_;
};

}  // namespace search
