#include "core/search_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "core/tokenizer.h"

namespace search {
namespace {

// Unique query terms in their original order.
std::vector<std::string> UniqueTerms(std::string_view query) {
  std::vector<std::string> terms;
  std::unordered_set<std::string> seen;
  detail::ForEachToken(query, [&terms, &seen](const std::string& token) {
    if (seen.insert(token).second) {
      terms.push_back(token);
    }
  });
  return terms;
}

// Lowercased copy of `text` with byte offsets preserved: the tokenizer folds
// only ASCII and Cyrillic, where a code point keeps its UTF-8 length. Offsets
// found in the copy are therefore valid in the original text.
std::string LowercaseCopy(std::string_view text) {
  std::string lowered;
  lowered.reserve(text.size());
  std::size_t pos = 0;
  while (pos < text.size()) {
    const std::size_t start = pos;
    const auto code_point = detail::DecodeUtf8(text, pos);
    if (!code_point) {
      // Invalid byte: copy one byte unchanged so offsets stay aligned.
      lowered.append(text.substr(start, pos - start));
      continue;
    }
    detail::AppendUtf8(detail::LowerCodePoint(*code_point), lowered);
  }
  return lowered;
}

// Collapses runs of whitespace into single spaces and trims the ends, so a
// snippet stays on one line in JSON and in the CLI.
std::string CollapseWhitespace(std::string_view window) {
  std::string collapsed;
  collapsed.reserve(window.size());
  bool pending_space = false;
  for (char character : window) {
    if (character == ' ' || character == '\t' || character == '\n' ||
        character == '\r') {
      pending_space = !collapsed.empty();
      continue;
    }
    if (pending_space) {
      collapsed.push_back(' ');
      pending_space = false;
    }
    collapsed.push_back(character);
  }
  return collapsed;
}

using RankedHit = std::pair<std::string, double>;

}  // namespace

SearchEngine::SearchEngine(std::string data_dir)
    : data_dir_(std::move(data_dir)) {}

std::vector<SearchResult> SearchEngine::Search(const InvertedIndex& index,
                                               std::string_view query) const {
  const std::vector<std::string> terms = UniqueTerms(query);
  if (terms.empty()) {
    return {};
  }
  const std::size_t documents = index.DocumentCount();
  if (documents == 0) {
    return {};
  }

  std::unordered_map<DocId, double> scores;
  for (const std::string& term : terms) {
    const std::vector<Posting> postings = index.Lookup(term);
    if (postings.empty()) {
      continue;
    }
    const double idf =
        std::log(1.0 + static_cast<double>(documents) /
                          static_cast<double>(postings.size()));
    for (const Posting& posting : postings) {
      const std::uint32_t length = index.DocumentLength(posting.doc_id);
      if (length == 0) {
        continue;
      }
      scores[posting.doc_id] +=
          (static_cast<double>(posting.term_freq) /
           static_cast<double>(length)) *
          idf;
    }
  }
  if (scores.empty()) {
    return {};
  }

  // Doc ids are resolved before sorting so equal scores are ordered by the
  // external doc_id string, which is what the contract documents. Internal
  // DocId order depends on insertion order and would leak it into the response.
  std::vector<RankedHit> ranked;
  ranked.reserve(scores.size());
  for (const auto& [doc_id, score] : scores) {
    ranked.emplace_back(index.DocumentId(doc_id), score);
  }
  std::sort(ranked.begin(), ranked.end(),
            [](const RankedHit& left, const RankedHit& right) {
              if (left.second != right.second) {
                return left.second > right.second;
              }
              return left.first < right.first;
            });

  const double best = ranked.front().second;
  const std::size_t count = std::min(kMaxResults, ranked.size());
  std::vector<SearchResult> results;
  results.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    SearchResult result;
    result.doc_id = ranked[i].first;
    result.score = ranked[i].second / best;
    result.snippet = Snippet(result.doc_id, terms);
    results.push_back(std::move(result));
  }
  return results;
}

std::string SearchEngine::Snippet(const std::string& doc_id,
                                  const std::vector<std::string>& terms) const {
  std::ifstream file(std::filesystem::path(data_dir_) / doc_id,
                     std::ios::binary);
  if (!file) {
    return {};
  }
  const std::string text((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
  if (text.empty()) {
    return {};
  }

  const std::string lowered = LowercaseCopy(text);
  std::size_t match = std::string::npos;
  std::size_t match_length = 0;
  for (const std::string& term : terms) {
    const std::size_t position = lowered.find(term);
    if (position != std::string::npos &&
        (match == std::string::npos || position < match)) {
      match = position;
      match_length = term.size();
    }
  }

  std::size_t begin = 0;
  std::size_t end = std::min(text.size(), kSnippetBefore + kSnippetAfter);
  if (match != std::string::npos) {
    begin = match > kSnippetBefore ? match - kSnippetBefore : 0;
    end = std::min(text.size(), match + match_length + kSnippetAfter);
  }
  // Never cut a multi-byte sequence in half.
  while (begin > 0 && (static_cast<unsigned char>(text[begin]) & 0xC0U) == 0x80U) {
    ++begin;
  }
  while (end < text.size() &&
         (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
    --end;
  }
  return CollapseWhitespace(std::string_view(text).substr(begin, end - begin));
}

}  // namespace search
