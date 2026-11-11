#include "integrations/ollama_client.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <exception>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/tokenizer.h"

namespace search {
namespace {

// Enough synonyms to broaden a query, few enough to keep ranking meaningful.
constexpr std::size_t kMaxExpandedTerms = 8;

// The prompt asks for a plain comma-separated list; the tokenizer below turns
// any list-like answer (commas, newlines, numbering) into terms anyway.
std::string BuildExpansionPrompt(std::string_view query) {
  std::string prompt =
      "Expand the search query into synonyms and related keywords. "
      "Answer with a comma-separated list of single words or short phrases. "
      "No numbering, no explanations.\nQuery: ";
  prompt.append(query);
  prompt += "\nKeywords:";
  return prompt;
}

// Tokens of the model answer that are not already part of the query. The same
// tokenizer as the index is used, so a term means the same thing everywhere.
std::vector<std::string> ParseExpandedTerms(std::string_view answer,
                                            std::string_view query) {
  std::unordered_set<std::string> seen;
  detail::ForEachToken(query, [&seen](const std::string& token) {
    seen.insert(token);
  });

  std::vector<std::string> terms;
  detail::ForEachTokenAt(
      answer, [&seen, &terms](const std::string& token, std::size_t) {
        if (terms.size() >= kMaxExpandedTerms) {
          return false;
        }
        if (seen.insert(token).second) {
          terms.push_back(token);
        }
        return terms.size() < kMaxExpandedTerms;
      });
  return terms;
}

}  // namespace

OllamaClient::OllamaClient(std::string base_url,
                           std::chrono::milliseconds timeout,
                           std::chrono::milliseconds generation_timeout)
    : base_url_(std::move(base_url)),
      timeout_(timeout),
      generation_timeout_(generation_timeout) {}

bool OllamaClient::IsAvailable() const {
  try {
    httplib::Client client(base_url_);
    client.set_connection_timeout(timeout_);
    client.set_read_timeout(timeout_);
    client.set_write_timeout(timeout_);

    const httplib::Result result = client.Get("/api/tags");
    return result && result->status == 200;
  } catch (const std::exception&) {
    return false;
  }
}

std::vector<std::string> OllamaClient::ExpandQuery(
    std::string_view query, std::string_view model) const {
  if (query.empty() || model.empty()) {
    return {};
  }
  try {
    httplib::Client client(base_url_);
    client.set_connection_timeout(timeout_);
    client.set_read_timeout(generation_timeout_);
    client.set_write_timeout(generation_timeout_);

    const nlohmann::json request = {
        {"model", std::string(model)},
        {"prompt", BuildExpansionPrompt(query)},
        // A single answer is enough; no token stream has to be parsed.
        {"stream", false},
        // Synonyms should be reproducible, not creative.
        {"options", {{"temperature", 0}}},
    };
    const httplib::Result result = client.Post(
        "/api/generate", request.dump(), "application/json");
    if (!result || result->status != 200) {
      return {};
    }

    const nlohmann::json body =
        nlohmann::json::parse(result->body, nullptr, false);
    if (body.is_discarded() || !body.is_object() ||
        !body.contains("response") || !body.at("response").is_string()) {
      return {};
    }
    return ParseExpandedTerms(body.at("response").get<std::string>(), query);
  } catch (const std::exception&) {
    return {};
  }
}

}  // namespace search
