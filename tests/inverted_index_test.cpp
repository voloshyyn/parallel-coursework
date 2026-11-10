#include "core/inverted_index.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/thread_pool.h"

namespace {

using search::DocId;
using search::InvertedIndex;
using search::Posting;

std::string DocName(int index) {
  return "doc" + std::to_string(index) + ".txt";
}

// Documents share a small vocabulary, so terms land in several shards and get
// different frequencies.
std::vector<std::string> Corpus(int documents) {
  std::vector<std::string> corpus;
  corpus.reserve(static_cast<std::size_t>(documents));
  for (int doc = 0; doc < documents; ++doc) {
    std::string text;
    for (int word = 0; word < 50; ++word) {
      text += "word";
      text += std::to_string((doc * 7 + word * 13) % 89);
      text += (word % 5 == 0) ? ". " : " ";
    }
    corpus.push_back(std::move(text));
  }
  return corpus;
}

// Postings identified by external document id, because parallel builds assign
// internal DocIds in a nondeterministic order.
std::vector<std::pair<std::string, std::uint32_t>> NormalizedPostings(
    const InvertedIndex& index, std::string_view term) {
  std::vector<std::pair<std::string, std::uint32_t>> postings;
  for (const Posting& posting : index.Lookup(term)) {
    postings.emplace_back(index.DocumentId(posting.doc_id), posting.term_freq);
  }
  std::sort(postings.begin(), postings.end());
  return postings;
}

}  // namespace

TEST(InvertedIndex, RejectsZeroShards) {
  EXPECT_THROW(InvertedIndex index(0), std::invalid_argument);
}

TEST(InvertedIndex, IndexesTermFrequencies) {
  InvertedIndex index;
  const DocId id = index.AddDocument("a.txt", "the cat and the dog");

  ASSERT_EQ(1U, index.Lookup("the").size());
  EXPECT_EQ(id, index.Lookup("the")[0].doc_id);
  EXPECT_EQ(2U, index.Lookup("the")[0].term_freq);
  EXPECT_EQ(1U, index.Lookup("cat")[0].term_freq);
  // Terms: the, cat, and, dog - one posting each.
  EXPECT_EQ(4U, index.TermCount());
  EXPECT_EQ(4U, index.PostingCount());
}

TEST(InvertedIndex, ReturnsNothingForUnknownTerm) {
  InvertedIndex index;
  index.AddDocument("a.txt", "the cat");

  EXPECT_TRUE(index.Lookup("zebra").empty());
}

TEST(InvertedIndex, IgnoresDocumentWithoutTokens) {
  InvertedIndex index;
  const DocId id = index.AddDocument("empty.txt", " \n\t.,- ");

  EXPECT_EQ(1U, index.DocumentCount());
  EXPECT_EQ("empty.txt", index.DocumentId(id));
  EXPECT_EQ(0U, index.DocumentLength(id));
  EXPECT_EQ(0U, index.PostingCount());
  EXPECT_TRUE(index.Lookup("the").empty());
}

TEST(InvertedIndex, CountsRepeatedWordsAndDocumentLength) {
  InvertedIndex index;
  const DocId id = index.AddDocument("a.txt", "Go go GO. go!");

  EXPECT_EQ(4U, index.DocumentLength(id));
  ASSERT_EQ(1U, index.Lookup("go").size());
  EXPECT_EQ(4U, index.Lookup("go")[0].term_freq);
}

TEST(InvertedIndex, ReportsDocumentMetadataInInsertionOrder) {
  InvertedIndex index;
  const DocId first = index.AddDocument("first.txt", "alpha");
  const DocId second = index.AddDocument("second.txt", "beta beta");

  EXPECT_EQ(0U, first);
  EXPECT_EQ(1U, second);
  EXPECT_EQ(2U, index.DocumentCount());
  EXPECT_EQ("first.txt", index.DocumentId(first));
  EXPECT_EQ("second.txt", index.DocumentId(second));
  EXPECT_THROW(index.DocumentId(2), std::out_of_range);
  EXPECT_THROW(index.DocumentLength(2), std::out_of_range);
}

TEST(InvertedIndex, RejectsDuplicateDocumentId) {
  InvertedIndex index;
  index.AddDocument("a.txt", "the cat");

  EXPECT_THROW(index.AddDocument("a.txt", "other text"), std::invalid_argument);
  EXPECT_EQ(1U, index.DocumentCount());
  EXPECT_EQ(1U, index.Lookup("cat").size());
  EXPECT_TRUE(index.Lookup("other").empty());
}

TEST(InvertedIndex, ClearDropsDocumentsAndPostings) {
  InvertedIndex index(4);
  index.AddDocument("a.txt", "the cat");
  index.AddDocument("b.txt", "the dog");

  index.Clear();

  EXPECT_EQ(0U, index.DocumentCount());
  EXPECT_EQ(0U, index.TermCount());
  EXPECT_EQ(0U, index.PostingCount());
  EXPECT_TRUE(index.Lookup("the").empty());

  const DocId id = index.AddDocument("a.txt", "the cat");
  EXPECT_EQ(0U, id);
  EXPECT_EQ(1U, index.DocumentCount());
  EXPECT_EQ(1U, index.Lookup("the").size());
}

TEST(InvertedIndex, SamePostingsForAnyShardCount) {
  const auto corpus = Corpus(60);
  InvertedIndex single_shard(1);
  InvertedIndex many_shards(64);
  for (std::size_t i = 0; i < corpus.size(); ++i) {
    const std::string name = DocName(static_cast<int>(i));
    single_shard.AddDocument(name, corpus[i]);
    many_shards.AddDocument(name, corpus[i]);
  }

  EXPECT_EQ(1U, single_shard.ShardCount());
  EXPECT_EQ(64U, many_shards.ShardCount());
  EXPECT_EQ(single_shard.TermCount(), many_shards.TermCount());
  EXPECT_EQ(single_shard.PostingCount(), many_shards.PostingCount());
  for (int term = 0; term < 89; ++term) {
    const std::string word = "word" + std::to_string(term);
    EXPECT_EQ(NormalizedPostings(single_shard, word),
              NormalizedPostings(many_shards, word))
        << "term: " << word;
  }
}

TEST(InvertedIndex, ParallelBuildMatchesSequentialBuild) {
  constexpr int kDocuments = 200;
  const auto corpus = Corpus(kDocuments);

  InvertedIndex sequential(8);
  for (int i = 0; i < kDocuments; ++i) {
    sequential.AddDocument(DocName(i), corpus[i]);
  }

  InvertedIndex parallel(8);
  {
    search::ThreadPool pool(4);
    for (int i = 0; i < kDocuments; ++i) {
      ASSERT_TRUE(pool.Submit(
          [&parallel, &corpus, i] { parallel.AddDocument(DocName(i), corpus[i]); }));
    }
  }

  EXPECT_EQ(sequential.DocumentCount(), parallel.DocumentCount());
  EXPECT_EQ(sequential.TermCount(), parallel.TermCount());
  EXPECT_EQ(sequential.PostingCount(), parallel.PostingCount());
  for (int term = 0; term < 89; ++term) {
    const std::string word = "word" + std::to_string(term);
    EXPECT_EQ(NormalizedPostings(sequential, word),
              NormalizedPostings(parallel, word))
        << "term: " << word;
  }
}

TEST(InvertedIndex, AcceptsWritesFromManyThreads) {
  constexpr int kThreads = 8;
  constexpr int kDocumentsPerThread = 100;
  InvertedIndex index(16);
  std::atomic<int> added{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&index, &added, thread] {
      for (int doc = 0; doc < kDocumentsPerThread; ++doc) {
        const int id = thread * kDocumentsPerThread + doc;
        index.AddDocument(DocName(id),
                          "common unique" + std::to_string(id) + " common");
        added.fetch_add(1);
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  const std::size_t documents =
      static_cast<std::size_t>(kThreads * kDocumentsPerThread);
  EXPECT_EQ(documents, static_cast<std::size_t>(added.load()));
  EXPECT_EQ(documents, index.DocumentCount());
  ASSERT_EQ(documents, index.Lookup("common").size());
  for (const Posting& posting : index.Lookup("common")) {
    EXPECT_EQ(2U, posting.term_freq);
  }
  for (std::size_t id = 0; id < documents; ++id) {
    const auto postings = index.Lookup("unique" + std::to_string(id));
    ASSERT_EQ(1U, postings.size()) << "unique term: " << id;
    EXPECT_EQ(1U, postings[0].term_freq);
  }
}

// The reader must check real postings while the build is still running. A
// single start barrier is not enough: it only aligns the threads, so the reader
// could look at an empty index and the writer could finish before the lookup.
// Instead the writer signals after the first batch and then waits for the
// reader's confirmation before adding the remaining documents.
TEST(InvertedIndex, AnswersLookupsWhileAnotherThreadIndexes) {
  constexpr int kDocuments = 300;
  constexpr int kFirstBatch = 10;
  InvertedIndex index(8);

  std::binary_semaphore first_batch_added{0};
  std::binary_semaphore lookup_confirmed{0};
  std::atomic<bool> finished{false};
  std::atomic<int> lookups{0};

  std::thread reader([&index, &first_batch_added, &lookup_confirmed,
                      &finished, &lookups] {
    first_batch_added.acquire();

    // This lookup is guaranteed to see the first batch, not an empty index.
    const std::vector<Posting> observed = index.Lookup("common");
    EXPECT_FALSE(observed.empty());
    for (const Posting& posting : observed) {
      EXPECT_LT(posting.doc_id, index.DocumentCount());
      EXPECT_GT(posting.term_freq, 0U);
    }
    lookups.fetch_add(1, std::memory_order_relaxed);
    lookup_confirmed.release();

    // Keep reading while the writer indexes the rest of the corpus.
    while (!finished.load(std::memory_order_acquire)) {
      for (const Posting& posting : index.Lookup("common")) {
        EXPECT_LT(posting.doc_id, index.DocumentCount());
        EXPECT_GT(posting.term_freq, 0U);
      }
      lookups.fetch_add(1, std::memory_order_relaxed);
    }
  });

  bool confirmed = false;
  for (int i = 0; i < kDocuments; ++i) {
    index.AddDocument(DocName(i), "common text");
    if (i + 1 == kFirstBatch) {
      first_batch_added.release();
      // The handshake is what actually orders the phases; the bounded wait only
      // keeps a broken reader from hanging the test until the CTest timeout.
      confirmed = lookup_confirmed.try_acquire_for(std::chrono::seconds(10));
      if (!confirmed) {
        break;
      }
      // Documents are still being added: the confirmed lookup saw a build in
      // progress, not the final index.
      EXPECT_LT(index.DocumentCount(), static_cast<std::size_t>(kDocuments));
    }
  }
  finished.store(true, std::memory_order_release);
  reader.join();

  EXPECT_TRUE(confirmed);
  EXPECT_EQ(static_cast<std::size_t>(kDocuments), index.DocumentCount());
  EXPECT_GT(lookups.load(), 0);
}
