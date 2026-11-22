#pragma once

#include <cstddef>
#include <string>

namespace search {

// Runtime configuration read from environment variables.
struct Config {
  int port = 8080;
  std::string ollama_url = "http://localhost:11434";
  std::string ollama_model = "qwen2.5:1.5b";
  std::string data_dir = "/data/docs";

  // Build the index from DATA_DIR right after HTTP starts, so a composed
  // deployment needs no manual POST /api/index.
  bool auto_index = true;
  std::size_t index_threads = 4;

  // WebSocket search runs off Crow's I/O threads on the project's pool.
  std::size_t search_workers = 4;
  std::size_t search_task_queue = 64;
  std::size_t search_connection_queue = 4;

  static Config FromEnvironment();
};

}  // namespace search
