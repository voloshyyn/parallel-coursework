#include "services/search_pipeline.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core/inverted_index.h"
#include "core/search_engine.h"
#include "test_support.h"

namespace {

using search::InvertedIndex;
using search::SearchEngine;
using search::SearchOutcome;
using search::SearchPipeline;
using search::test_support::TempDir;

void AddDocuments(InvertedIndex& index, const TempDir& dir,
                  const std::vector<std::string>& names,
                  const std::vector<std::string>& texts) {
  for (std::size_t i = 0; i < names.size(); ++i) {
    dir.Write(names[i], texts[i]);
    index.AddDocument(names[i], texts[i]);
  }
}

}  // namespace

TEST(SearchPipeline, SearchesTheOriginalQueryWithoutAnExpander) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  dir.Write("b.txt", "whale");
  InvertedIndex index;
  AddDocuments(index, dir, {"a.txt", "b.txt"}, {"alpha", "whale"});
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(engine);

  const SearchOutcome outcome = pipeline.Run(index, "alpha");

  EXPECT_EQ("alpha", outcome.original_query);
  EXPECT_TRUE(outcome.expanded_terms.empty());
  ASSERT_EQ(1U, outcome.results.size());
  EXPECT_EQ("a.txt", outcome.results[0].doc_id);
  EXPECT_GE(outcome.search_time_ms, 0);
}

TEST(SearchPipeline, SearchesExpandedTermsTogetherWithTheQuery) {
  TempDir dir;
  dir.Write("a.txt", "fish");
  dir.Write("b.txt", "whale");
  InvertedIndex index;
  AddDocuments(index, dir, {"a.txt", "b.txt"}, {"fish", "whale"});
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(
      engine, [](std::string_view) {
        return std::vector<std::string>{"whale"};
      });

  const SearchOutcome outcome = pipeline.Run(index, "fish");

  EXPECT_EQ("fish", outcome.original_query);
  ASSERT_EQ(1U, outcome.expanded_terms.size());
  EXPECT_EQ("whale", outcome.expanded_terms[0]);
  // Both the original term and the synonym match.
  ASSERT_EQ(2U, outcome.results.size());
  EXPECT_EQ("a.txt", outcome.results[0].doc_id);
  EXPECT_EQ("b.txt", outcome.results[1].doc_id);
}

TEST(SearchPipeline, FallsBackToTheOriginalQueryWhenExpansionIsEmpty) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  InvertedIndex index;
  AddDocuments(index, dir, {"a.txt"}, {"alpha"});
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(
      engine, [](std::string_view) { return std::vector<std::string>{}; });

  const SearchOutcome outcome = pipeline.Run(index, "alpha");

  EXPECT_TRUE(outcome.expanded_terms.empty());
  ASSERT_EQ(1U, outcome.results.size());
  EXPECT_EQ("a.txt", outcome.results[0].doc_id);
}

// OllamaClient never throws, but an injected expander might; the search still
// has to answer.
TEST(SearchPipeline, FallsBackWhenExpansionThrows) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  InvertedIndex index;
  AddDocuments(index, dir, {"a.txt"}, {"alpha"});
  const SearchEngine engine(dir.path().string());
  const SearchPipeline pipeline(
      engine, [](std::string_view) -> std::vector<std::string> {
        throw std::runtime_error("expansion failed");
      });

  const SearchOutcome outcome = pipeline.Run(index, "alpha");

  EXPECT_TRUE(outcome.expanded_terms.empty());
  ASSERT_EQ(1U, outcome.results.size());
  EXPECT_EQ("a.txt", outcome.results[0].doc_id);
}

TEST(SearchPipeline, DoesNotExpandAnEmptyQuery) {
  TempDir dir;
  dir.Write("a.txt", "alpha");
  InvertedIndex index;
  AddDocuments(index, dir, {"a.txt"}, {"alpha"});
  const SearchEngine engine(dir.path().string());
  int calls = 0;
  const SearchPipeline pipeline(engine, [&calls](std::string_view) {
    ++calls;
    return std::vector<std::string>{"whale"};
  });

  const SearchOutcome outcome = pipeline.Run(index, "");

  EXPECT_EQ(0, calls);
  EXPECT_TRUE(outcome.results.empty());
  EXPECT_EQ(0, outcome.search_time_ms);
}
