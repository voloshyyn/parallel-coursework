#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/inverted_index.h"

namespace search {

enum class IndexState { kEmpty, kBuilding, kReady };

struct BuildStats {
  std::size_t documents_indexed = 0;
  std::int64_t build_time_ms = 0;
  std::size_t threads_used = 0;
};

// Owns the inverted index. A build creates a new index, fills it from worker
// threads and publishes it atomically, so readers never observe a half-built
// index and a rebuild does not have to clear the live one.
class IndexService {
 public:
  // Reads one document from disk. Injectable so tests can control timing and
  // failures; the default implementation reads the file as binary.
  using ReadDocument = std::function<std::string(const std::filesystem::path&)>;

  explicit IndexService(std::string data_dir, ReadDocument read_document = {});

  IndexService(const IndexService&) = delete;
  IndexService& operator=(const IndexService&) = delete;

  // Builds a new index with `threads` workers and returns its stats. Returns
  // nothing when another build is already running; the live index stays
  // untouched in that case. Throws std::invalid_argument for `threads == 0`
  // and std::runtime_error when a document cannot be indexed.
  std::optional<BuildStats> Build(std::size_t threads);

  IndexState State() const noexcept {
    return state_.load(std::memory_order_acquire);
  }

  // Index currently served; empty until the first successful build.
  std::shared_ptr<const InvertedIndex> Snapshot() const;

  // Stats of the last successful build; zeros when nothing has been built.
  BuildStats Stats() const;

 private:
  std::vector<std::filesystem::path> ListDocuments() const;

  const std::string data_dir_;
  const ReadDocument read_document_;

  std::atomic<IndexState> state_{IndexState::kEmpty};
  std::atomic<bool> building_{false};

  mutable std::mutex index_mutex_;
  std::shared_ptr<const InvertedIndex> index_;

  mutable std::mutex stats_mutex_;
  BuildStats stats_;
};

}  // namespace search
