#pragma once

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

// Index and stats of one successful build. Both are published together, so a
// reader can never combine the document count of one build with the thread
// count of another.
struct PublishedIndex {
  // Null until the first successful build.
  std::shared_ptr<const InvertedIndex> index;
  // Zeros until the first successful build.
  BuildStats stats;
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

  IndexState State() const;

  // Generation currently served. Both members always belong to the same
  // successful build; a failed rebuild keeps the previous generation.
  PublishedIndex Snapshot() const;

 private:
  std::vector<std::filesystem::path> ListDocuments() const;

  const std::string data_dir_;
  const ReadDocument read_document_;

  // Guards the state machine and the published generation. Only short,
  // non-blocking transitions use it: files are listed, read and indexed while
  // the lock is not held.
  mutable std::mutex state_mutex_;
  IndexState state_ = IndexState::kEmpty;
  bool building_ = false;
  PublishedIndex published_;
};

}  // namespace search
