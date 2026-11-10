#include "services/index_service.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "core/thread_pool.h"

namespace search {
namespace {

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open document: " + path.string());
  }
  std::string text((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  if (file.bad()) {
    throw std::runtime_error("cannot read document: " + path.string());
  }
  return text;
}

}  // namespace

IndexService::IndexService(std::string data_dir, ReadDocument read_document)
    : data_dir_(std::move(data_dir)),
      read_document_(read_document ? std::move(read_document) : ReadFile) {}

std::optional<BuildStats> IndexService::Build(std::size_t threads) {
  if (threads == 0) {
    throw std::invalid_argument("index build requires at least one thread");
  }
  // Only one build at a time: a second one would compete for memory and make
  // /api/stats ambiguous.
  bool expected = false;
  if (!building_.compare_exchange_strong(expected, true)) {
    return std::nullopt;
  }
  state_.store(IndexState::kBuilding, std::memory_order_release);

  const auto started = std::chrono::steady_clock::now();
  try {
    const std::vector<std::filesystem::path> documents = ListDocuments();
    auto index = std::make_shared<InvertedIndex>();

    // Workers report the first failure instead of throwing: ThreadPool would
    // log and swallow the exception, and the build must fail as a whole.
    std::mutex error_mutex;
    std::string first_error;
    {
      ThreadPool pool(threads);
      for (const std::filesystem::path& path : documents) {
        const std::string doc_id = path.filename().string();
        const bool submitted = pool.Submit([&index, &error_mutex, &first_error,
                                            path, doc_id, this] {
          try {
            index->AddDocument(doc_id, read_document_(path));
          } catch (const std::exception& error) {
            std::lock_guard<std::mutex> lock(error_mutex);
            if (first_error.empty()) {
              first_error = std::string(path.filename().string()) + ": " +
                            error.what();
            }
          }
        });
        if (!submitted) {
          throw std::runtime_error("thread pool rejected an indexing task");
        }
      }
    }

    if (!first_error.empty()) {
      throw std::runtime_error("index build failed: " + first_error);
    }

    const auto elapsed = std::chrono::steady_clock::now() - started;
    const BuildStats stats{
        .documents_indexed = index->DocumentCount(),
        .build_time_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                .count(),
        .threads_used = threads,
    };

    {
      std::lock_guard<std::mutex> lock(index_mutex_);
      index_ = std::move(index);
    }
    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      stats_ = stats;
    }
    building_.store(false, std::memory_order_release);
    state_.store(IndexState::kReady, std::memory_order_release);
    return stats;
  } catch (...) {
    building_.store(false, std::memory_order_release);
    // A failed rebuild must not take the previously built index offline.
    state_.store(Snapshot() ? IndexState::kReady : IndexState::kEmpty,
                 std::memory_order_release);
    throw;
  }
}

std::shared_ptr<const InvertedIndex> IndexService::Snapshot() const {
  std::lock_guard<std::mutex> lock(index_mutex_);
  return index_;
}

BuildStats IndexService::Stats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return stats_;
}

std::vector<std::filesystem::path> IndexService::ListDocuments() const {
  std::vector<std::filesystem::path> documents;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(data_dir_)) {
    if (entry.is_regular_file()) {
      documents.push_back(entry.path());
    }
  }
  // Stable order keeps doc ids reproducible between runs.
  std::sort(documents.begin(), documents.end());
  return documents;
}

}  // namespace search
