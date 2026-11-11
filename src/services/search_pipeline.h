#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/search_engine.h"

namespace search {

// Produces extra terms for a query. An empty result means "no expansion
// available"; the pipeline then searches with the original query only.
using QueryExpander =
    std::function<std::vector<std::string>(std::string_view query)>;

// Outcome of one pipeline run. `search_time_ms` covers only the keyword search,
// matching the `time_ms` field of the REST contract.
struct SearchOutcome {
  std::string original_query;
  std::vector<std::string> expanded_terms;
  std::vector<SearchResult> results;
  std::int64_t search_time_ms = 0;
};

// Query expansion followed by keyword search. Expansion is best effort: a
// missing expander, an empty answer or an exception all leave the search on the
// original query, which is what docs/api.md promises when Ollama is
// unavailable.
class SearchPipeline {
 public:
  explicit SearchPipeline(const SearchEngine& engine,
                          QueryExpander expander = {});

  // `index` must stay alive for the duration of the call.
  SearchOutcome Run(const InvertedIndex& index, std::string_view query) const;

 private:
  const SearchEngine& engine_;
  const QueryExpander expander_;
};

}  // namespace search
