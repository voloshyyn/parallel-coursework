#include "integrations/ollama_client.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <cstddef>
#include <exception>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/tokenizer.h"

namespace search {
namespace {

// Enough synonyms to broaden a query, few enough to keep ranking meaningful.
constexpr std::size_t kMaxExpandedTerms = 8;

// Length of the UTF-8 bullet at `pos`, or zero when there is none.
std::size_t BulletLength(std::string_view text, std::size_t pos) {
  if (pos < text.size() && (text[pos] == '-' || text[pos] == '*' ||
                            text[pos] == '+')) {
    return 1;
  }
  const auto at = [&text, pos](std::size_t offset) {
    return static_cast<unsigned char>(text[pos + offset]);
  };
  if (pos + 2 < text.size() && at(0) == 0xE2U && at(1) == 0x80U &&
      (at(2) == 0xA2U || at(2) == 0xA3U || at(2) == 0x93U ||
       at(2) == 0x94U)) {
    return 3;  // •, ‣, –, —
  }
  if (pos + 1 < text.size() && at(0) == 0xC2U && at(1) == 0xB7U) {
    return 2;  // ·
  }
  return 0;
}

// Skips a leading list marker at `pos`: a bullet or "1." / "1)" / "1:".
// The number must be short (at most three digits) and followed by whitespace,
// so a year such as "2020." or a token such as "C++20" is left intact. On
// success `pos` points past the marker and its trailing spaces.
bool SkipListMarker(std::string_view text, std::size_t& pos) {
  std::size_t index = pos;
  if (index >= text.size()) {
    return false;
  }

  if (const std::size_t bullet = BulletLength(text, index); bullet != 0) {
    std::size_t after = index + bullet;
    if (after < text.size() &&
        (text[after] == ' ' || text[after] == '\t')) {
      while (after < text.size() &&
             (text[after] == ' ' || text[after] == '\t')) {
        ++after;
      }
      pos = after;
      return true;
    }
    return false;
  }

  std::size_t digits = 0;
  while (index + digits < text.size() && digits < 3 &&
         std::isdigit(static_cast<unsigned char>(text[index + digits]))) {
    ++digits;
  }
  if (digits == 0 || index + digits >= text.size()) {
    return false;
  }
  const char marker = text[index + digits];
  if (marker != '.' && marker != ')' && marker != ':') {
    return false;
  }
  std::size_t after = index + digits + 1;
  // "3.14" is a number, not a list item: a marker needs whitespace after it.
  if (after >= text.size() || (text[after] != ' ' && text[after] != '\t')) {
    return false;
  }
  while (after < text.size() &&
         (text[after] == ' ' || text[after] == '\t')) {
    ++after;
  }
  pos = after;
  return true;
}

// Drops numbered and bulleted list markers before tokenization. Models answer
// with "1. whale\n2. ocean" often enough that the numbers would otherwise take
// slots in the eight-term limit. Content numbers such as "2020" stay.
std::string StripListMarkers(std::string_view answer) {
  std::string cleaned;
  cleaned.reserve(answer.size());

  std::size_t line_start = 0;
  while (line_start <= answer.size()) {
    std::size_t line_end = answer.find('\n', line_start);
    if (line_end == std::string_view::npos) {
      line_end = answer.size();
    }
    std::string_view line = answer.substr(line_start, line_end - line_start);

    std::size_t index = 0;
    while (index < line.size() &&
           (line[index] == ' ' || line[index] == '\t')) {
      ++index;
    }
    SkipListMarker(line, index);

    for (std::size_t i = index; i < line.size(); ++i) {
      cleaned.push_back(line[i]);
      if (line[i] != ',') {
        continue;
      }
      // A single-line list may separate items with commas: "1. whale, 2. sea".
      std::size_t next = i + 1;
      while (next < line.size() &&
             (line[next] == ' ' || line[next] == '\t')) {
        ++next;
      }
      std::size_t marker = next;
      if (SkipListMarker(line, marker)) {
        cleaned.push_back(' ');
        i = marker - 1;
      }
    }
    if (line_end < answer.size()) {
      cleaned.push_back('\n');
    }
    if (line_end == answer.size()) {
      break;
    }
    line_start = line_end + 1;
  }
  return cleaned;
}

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

  const std::string cleaned = StripListMarkers(answer);
  std::vector<std::string> terms;
  detail::ForEachTokenAt(
      cleaned, [&seen, &terms](const std::string& token, std::size_t) {
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
    client.set_max_timeout(timeout_);

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
    // Read/write timeouts are per operation, so a model that keeps sending
    // tokens every few milliseconds would never hit them. max_timeout bounds
    // the whole request and makes the fallback deadline real.
    client.set_max_timeout(generation_timeout_);

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
