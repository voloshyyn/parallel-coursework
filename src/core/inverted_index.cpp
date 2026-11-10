#include "core/inverted_index.h"

#include "core/tokenizer.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace search {

InvertedIndex::InvertedIndex(std::size_t shard_count) {
  if (shard_count == 0) {
    throw std::invalid_argument("inverted index requires at least one shard");
  }

  shards_.reserve(shard_count);
  for (std::size_t i = 0; i < shard_count; ++i) {
    // std::shared_mutex is not movable, so shards are allocated individually.
    shards_.push_back(std::make_unique<Shard>());
  }
}

DocId InvertedIndex::AddDocument(std::string doc_id, std::string_view text) {
  std::unordered_map<std::string, std::uint32_t> frequencies;
  std::uint32_t length = 0;
  detail::ForEachToken(text, [&frequencies, &length](const std::string& token) {
    ++length;
    ++frequencies[token];
  });

  const DocId id = ReserveDocument(std::move(doc_id), length);

  // Group terms by shard first: each shard is then locked once per document
  // instead of once per distinct term.
  std::vector<std::vector<std::pair<const std::string*, std::uint32_t>>>
      per_shard(shards_.size());
  for (const auto& entry : frequencies) {
    per_shard[ShardIndex(entry.first)].emplace_back(&entry.first,
                                                    entry.second);
  }

  for (std::size_t i = 0; i < shards_.size(); ++i) {
    if (per_shard[i].empty()) {
      continue;
    }
    std::unique_lock<std::shared_mutex> lock(shards_[i]->mutex);
    for (const auto& entry : per_shard[i]) {
      shards_[i]->postings[*entry.first].push_back(Posting{id, entry.second});
      posting_count_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  return id;
}

std::vector<Posting> InvertedIndex::Lookup(std::string_view term) const {
  const std::size_t shard = ShardIndex(term);
  std::shared_lock<std::shared_mutex> lock(shards_[shard]->mutex);
  const auto it = shards_[shard]->postings.find(std::string(term));
  if (it == shards_[shard]->postings.end()) {
    return {};
  }
  return it->second;
}

std::size_t InvertedIndex::DocumentCount() const {
  std::lock_guard<std::mutex> lock(documents_mutex_);
  return documents_.size();
}

std::string InvertedIndex::DocumentId(DocId doc_id) const {
  std::lock_guard<std::mutex> lock(documents_mutex_);
  if (doc_id >= documents_.size()) {
    throw std::out_of_range("unknown document id");
  }
  return documents_[doc_id];
}

std::uint32_t InvertedIndex::DocumentLength(DocId doc_id) const {
  std::lock_guard<std::mutex> lock(documents_mutex_);
  if (doc_id >= document_lengths_.size()) {
    throw std::out_of_range("unknown document id");
  }
  return document_lengths_[doc_id];
}

std::size_t InvertedIndex::TermCount() const {
  std::size_t total = 0;
  for (const std::unique_ptr<Shard>& shard : shards_) {
    std::shared_lock<std::shared_mutex> lock(shard->mutex);
    total += shard->postings.size();
  }
  return total;
}

std::size_t InvertedIndex::PostingCount() const {
  return posting_count_.load(std::memory_order_relaxed);
}

void InvertedIndex::Clear() {
  {
    std::lock_guard<std::mutex> lock(documents_mutex_);
    documents_.clear();
    document_lengths_.clear();
    doc_ids_by_name_.clear();
  }
  for (const std::unique_ptr<Shard>& shard : shards_) {
    std::unique_lock<std::shared_mutex> lock(shard->mutex);
    shard->postings.clear();
  }
  posting_count_.store(0, std::memory_order_relaxed);
}

std::size_t InvertedIndex::ShardIndex(std::string_view term) const {
  return std::hash<std::string_view>{}(term) % shards_.size();
}

DocId InvertedIndex::ReserveDocument(std::string doc_id,
                                     std::uint32_t length) {
  std::lock_guard<std::mutex> lock(documents_mutex_);
  if (doc_ids_by_name_.contains(doc_id)) {
    throw std::invalid_argument("document is already indexed: " + doc_id);
  }
  if (documents_.size() >=
      static_cast<std::size_t>(std::numeric_limits<DocId>::max())) {
    throw std::length_error("inverted index exhausted document ids");
  }

  const auto id = static_cast<DocId>(documents_.size());
  documents_.push_back(std::move(doc_id));
  document_lengths_.push_back(length);
  doc_ids_by_name_.emplace(documents_.back(), id);
  return id;
}

}  // namespace search
