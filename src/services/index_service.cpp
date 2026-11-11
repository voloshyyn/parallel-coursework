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
  {
    // Checking `building_` and entering the building state happen under the
    // same lock, so two builds can never run at once.
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (building_) {
      return std::nullopt;
    }
    building_ = true;
    state_ = IndexState::kBuilding;
  }

  // Everything below runs without the state lock: listing, reading and
  // indexing files must not block readers of the state.
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
      // Publishing the index and its stats and leaving the building state is
      // one transition: the next build can only start afterwards, and a reader
      // sees the new generation together with the final state.
      std::lock_guard<std::mutex> lock(state_mutex_);
      published_.index = std::move(index);
      published_.stats = stats;
      building_ = false;
      state_ = IndexState::kReady;
    }
    return stats;
  } catch (...) {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      building_ = false;
      // A failed first build leaves nothing to serve; a failed rebuild must not
      // take the previously published generation offline.
      state_ = published_.index ? IndexState::kReady : IndexState::kEmpty;
    }
    throw;
  }
}

IndexState IndexService::State() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return state_;
}

PublishedIndex IndexService::Snapshot() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return published_;
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
