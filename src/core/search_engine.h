#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/inverted_index.h"

namespace search {

// One hit of POST /api/search.
struct SearchResult {
  std::string doc_id;
  double score = 0.0;
  std::string snippet;
};

// Keyword search over an inverted index.
//
// Scoring is TF-IDF: each query term contributes
// `term_freq / document_length * idf(term)`, where
// `idf = log(1 + documents / documents_containing_term)`. Scores are divided by
// the best one, so the response always carries values in (0, 1]. Equal scores
// are ordered by doc_id, which makes the ranking deterministic.
class SearchEngine {
 public:
  static constexpr std::size_t kMaxResults = 10;
  // Characters taken from the document around the first matched term.
  static constexpr std::size_t kSnippetBefore = 80;
  static constexpr std::size_t kSnippetAfter = 120;

  explicit SearchEngine(std::string data_dir);

  // `index` is a snapshot owned by IndexService; returns at most kMaxResults
  // hits. Snippets are read from `data_dir` on demand, so a hit whose file is
  // gone still has an empty snippet instead of failing the whole search.
  std::vector<SearchResult> Search(const InvertedIndex& index,
                                   std::string_view query) const;

 private:
  std::string Snippet(const std::string& doc_id,
                      const std::vector<std::string>& terms) const;

  std::string data_dir_;
};

}  // namespace search
