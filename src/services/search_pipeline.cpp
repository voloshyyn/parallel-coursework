#include "services/search_pipeline.h"

#include <chrono>
#include <exception>
#include <utility>

namespace search {

SearchPipeline::SearchPipeline(const SearchEngine& engine,
                               QueryExpander expander)
    : engine_(engine), expander_(std::move(expander)) {}

SearchOutcome SearchPipeline::Run(const InvertedIndex& index,
                                  std::string_view query) const {
  SearchOutcome outcome;
  outcome.original_query = std::string(query);
  if (query.empty()) {
    return outcome;
  }

  if (expander_) {
    try {
      outcome.expanded_terms = expander_(query);
    } catch (const std::exception&) {
      // Expansion is an optimization; a failure must not fail the search.
      outcome.expanded_terms.clear();
    }
  }

  // The search engine tokenizes the string, so joining original and expanded
  // terms searches for their union.
  std::string effective_query = outcome.original_query;
  for (const std::string& term : outcome.expanded_terms) {
    if (term.empty()) {
      continue;
    }
    effective_query.push_back(' ');
    effective_query.append(term);
  }

  const auto started = std::chrono::steady_clock::now();
  outcome.results = engine_.Search(index, effective_query);
  outcome.search_time_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started)
          .count();
  return outcome;
}

}  // namespace search
