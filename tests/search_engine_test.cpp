#include "core/search_engine.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/inverted_index.h"
#include "test_support.h"

namespace {

using search::DocId;
using search::InvertedIndex;
using search::SearchEngine;
using search::SearchResult;
using search::test_support::TempDir;

void AddDocuments(InvertedIndex& index, TempDir& dir,
                  const std::vector<std::string>& names,
                  const std::vector<std::string>& texts) {
  for (std::size_t i = 0; i < names.size(); ++i) {
    dir.Write(names[i], texts[i]);
    index.AddDocument(names[i], texts[i]);
  }
}

std::vector<std::string> DocIds(const std::vector<SearchResult>& results) {
  std::vector<std::string> ids;
  ids.reserve(results.size());
  for (const SearchResult& result : results) {
    ids.push_back(result.doc_id);
  }
  return ids;
}

}  // namespace

TEST(SearchEngine, ReturnsNothingForEmptyQuery) {
  TempDir dir;
  InvertedIndex index;
  index.AddDocument("a.txt", "alpha beta");
  const SearchEngine engine(dir.path().string());

  EXPECT_TRUE(engine.Search(index, "").empty());
  EXPECT_TRUE(engine.Search(index, "   !!! ;;").empty());
}

TEST(SearchEngine, ReturnsNothingForUnknownTerm) {
  TempDir dir;
  InvertedIndex index;
  index.AddDocument("a.txt", "alpha beta");
  const SearchEngine engine(dir.path().string());

  EXPECT_TRUE(engine.Search(index, "zebra").empty());
}

TEST(SearchEngine, ReturnsNothingForEmptyIndex) {
  TempDir dir;
  const InvertedIndex index;
  const SearchEngine engine(dir.path().string());

  EXPECT_TRUE(engine.Search(index, "whale").empty());
}

TEST(SearchEngine, RanksByTermFrequencyAndNormalizesScore) {
  TempDir dir;
  InvertedIndex index;
  AddDocuments(index, dir, {"a.txt", "b.txt", "c.txt"},
               {"whale whale whale", "whale", "whale fish"});
  const SearchEngine engine(dir.path().string());

  const std::vector<SearchResult> results = engine.Search(index, "whale");

  ASSERT_EQ(3U, results.size());
  EXPECT_EQ("a.txt", results[0].doc_id);
  EXPECT_EQ("b.txt", results[1].doc_id);
  EXPECT_EQ("c.txt", results[2].doc_id);
  EXPECT_DOUBLE_EQ(1.0, results[0].score);
  EXPECT_DOUBLE_EQ(1.0, results[1].score);
  EXPECT_DOUBLE_EQ(0.5, results[2].score);
}

TEST(SearchEngine, BreaksTiesByDocumentId) {
  TempDir dir;
  InvertedIndex index;
  AddDocuments(index, dir, {"d.txt", "b.txt", "c.txt", "a.txt"},
               {"same text", "same text", "same text", "same text"});
  const SearchEngine engine(dir.path().string());

  const std::vector<SearchResult> results = engine.Search(index, "same");

  ASSERT_EQ(4U, results.size());
  EXPECT_EQ((std::vector<std::string>{"a.txt", "b.txt", "c.txt", "d.txt"}),
            DocIds(results));
}

TEST(SearchEngine, AddsScoresOfSeveralQueryTerms) {
  TempDir dir;
  InvertedIndex index;
  AddDocuments(index, dir, {"a.txt", "b.txt"}, {"alpha beta", "alpha"});
  const SearchEngine engine(dir.path().string());

  const std::vector<SearchResult> results = engine.Search(index, "alpha beta");

  ASSERT_EQ(2U, results.size());
  EXPECT_EQ("a.txt", results[0].doc_id);
  EXPECT_EQ("b.txt", results[1].doc_id);
  EXPECT_GT(results[0].score, results[1].score);
  EXPECT_DOUBLE_EQ(1.0, results[0].score);
}

TEST(SearchEngine, IgnoresRepeatedQueryTerms) {
  TempDir dir;
  InvertedIndex index;
  AddDocuments(index, dir, {"a.txt", "b.txt"}, {"alpha beta", "alpha"});
  const SearchEngine engine(dir.path().string());

  const std::vector<SearchResult> single = engine.Search(index, "alpha");
  const std::vector<SearchResult> repeated =
      engine.Search(index, "alpha alpha alpha");

  ASSERT_EQ(single.size(), repeated.size());
  for (std::size_t i = 0; i < single.size(); ++i) {
    EXPECT_EQ(single[i].doc_id, repeated[i].doc_id);
    EXPECT_DOUBLE_EQ(single[i].score, repeated[i].score);
  }
}

TEST(SearchEngine, LimitsResults) {
  TempDir dir;
  InvertedIndex index;
  std::vector<std::string> names;
  std::vector<std::string> texts;
  for (int i = 0; i < 25; ++i) {
    names.push_back("doc" + std::to_string(i) + ".txt");
    texts.push_back("common word");
  }
  AddDocuments(index, dir, names, texts);
  const SearchEngine engine(dir.path().string());

  const std::vector<SearchResult> results = engine.Search(index, "common");

  EXPECT_EQ(SearchEngine::kMaxResults, results.size());
  for (const SearchResult& result : results) {
    EXPECT_DOUBLE_EQ(1.0, result.score);
  }
}

TEST(SearchEngine, SnippetSurroundsMatchedTerm) {
  TempDir dir;
  InvertedIndex index;
  const std::string prefix(200, 'x');
  const std::string text =
      prefix + "\nthe Whale lives in the deep ocean\n" + prefix;
  AddDocuments(index, dir, {"a.txt"}, {text});
  const SearchEngine engine(dir.path().string());

  const std::vector<SearchResult> results = engine.Search(index, "whale");

  ASSERT_EQ(1U, results.size());
  EXPECT_NE(std::string::npos, results[0].snippet.find("Whale"));
  EXPECT_EQ(std::string::npos, results[0].snippet.find('\n'));
  // The window is `kSnippetBefore` bytes before the term, the term itself and
  // `kSnippetAfter` bytes after it; whitespace collapsing never grows it.
  EXPECT_LE(results[0].snippet.size(),
            SearchEngine::kSnippetBefore + SearchEngine::kSnippetAfter + 5);
}

TEST(SearchEngine, HitWithoutFileOnDiskHasEmptySnippet) {
  TempDir dir;
  InvertedIndex index;
  // The document is indexed from memory but no file exists on disk: the hit is
  // still returned, only the snippet is empty.
  index.AddDocument("missing.txt", "ghost");
  const SearchEngine engine(dir.path().string());

  const std::vector<SearchResult> results = engine.Search(index, "ghost");

  ASSERT_EQ(1U, results.size());
  EXPECT_EQ("missing.txt", results[0].doc_id);
  EXPECT_TRUE(results[0].snippet.empty());
}
