#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace search {

// Internal document identifier, assigned by InvertedIndex in insertion order.
using DocId = std::uint32_t;

// One document that contains a term and how many times the term occurs in it.
struct Posting {
  DocId doc_id = 0;
  std::uint32_t term_freq = 0;
};

// Sharded inverted index mapping `term -> postings`.
//
// Terms are hashed into `shard_count` shards, each guarded by its own
// std::shared_mutex: concurrent writers touch different shards most of the
// time, and readers take only shared locks. No operation holds more than one
// lock at a time, so the index cannot deadlock.
//
// All public members are thread-safe, except Clear(), which must not run
// concurrently with AddDocument().
class InvertedIndex {
 public:
  static constexpr std::size_t kDefaultShardCount = 32;

  // Throws std::invalid_argument when shard_count is zero.
  explicit InvertedIndex(std::size_t shard_count = kDefaultShardCount);

  InvertedIndex(const InvertedIndex&) = delete;
  InvertedIndex& operator=(const InvertedIndex&) = delete;

  // Tokenizes `text` and records a posting for every term it contains.
  // Thread-safe: many threads may index different documents at once.
  //
  // Throws std::invalid_argument when `doc_id` is already indexed; call Clear()
  // before rebuilding the index from scratch.
  DocId AddDocument(std::string doc_id, std::string_view text);

  // Returns the postings of a normalized term (as produced by Tokenize).
  // Returns an empty vector for an unknown term.
  std::vector<Posting> Lookup(std::string_view term) const;

  std::size_t DocumentCount() const;
  // External document id assigned to a DocId. Throws std::out_of_range when
  // the id is unknown.
  std::string DocumentId(DocId doc_id) const;
  // Number of tokens the document contained, counting repeats.
  std::uint32_t DocumentLength(DocId doc_id) const;

  // Number of distinct terms and of `(term, document)` pairs in the index.
  std::size_t TermCount() const;
  std::size_t PostingCount() const;

  std::size_t ShardCount() const noexcept { return shards_.size(); }

  // Drops every document and posting. Not safe to run during AddDocument().
  void Clear();

 private:
  struct Shard {
    mutable std::shared_mutex mutex;
    std::unordered_map<std::string, std::vector<Posting>> postings;
  };

  std::size_t ShardIndex(std::string_view term) const;
  // Appends document metadata and returns the assigned DocId.
  DocId ReserveDocument(std::string doc_id, std::uint32_t length);

  std::vector<std::unique_ptr<Shard>> shards_;

  mutable std::mutex documents_mutex_;
  std::vector<std::string> documents_;
  std::vector<std::uint32_t> document_lengths_;
  std::unordered_map<std::string, DocId> doc_ids_by_name_;

  std::atomic<std::size_t> posting_count_{0};
};

}  // namespace search
