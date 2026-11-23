#include "config.h"

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace search {
namespace {

std::string ReadEnv(const char* name, const std::string& fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return fallback;
  }
  return value;
}

int ReadPort(const std::string& text) {
  int port = 0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto [ptr, error] = std::from_chars(begin, end, port);
  if (error != std::errc() || ptr != end || port < 1 || port > 65535) {
    throw std::runtime_error("PORT must be an integer in 1..65535: " + text);
  }
  return port;
}

// Reads a positive size for limits such as INDEX_THREADS.
std::size_t ReadPositive(const char* name, std::size_t fallback,
                         std::size_t maximum) {
  const std::string text = ReadEnv(name, "");
  if (text.empty()) {
    return fallback;
  }
  unsigned long long value = 0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto [ptr, error] = std::from_chars(begin, end, value);
  if (error != std::errc() || ptr != end || value < 1 || value > maximum) {
    throw std::runtime_error(std::string(name) + " must be in 1.." +
                             std::to_string(maximum) + ": " + text);
  }
  return static_cast<std::size_t>(value);
}

bool ReadBool(const char* name, bool fallback) {
  const std::string text = ReadEnv(name, "");
  if (text.empty()) {
    return fallback;
  }
  if (text == "1" || text == "true" || text == "TRUE" || text == "yes") {
    return true;
  }
  if (text == "0" || text == "false" || text == "FALSE" || text == "no") {
    return false;
  }
  throw std::runtime_error(std::string(name) +
                           " must be one of 0/1/true/false: " + text);
}

}  // namespace

Config Config::FromEnvironment() {
  Config config;
  config.port = ReadPort(ReadEnv("PORT", std::to_string(config.port)));
  config.ollama_url = ReadEnv("OLLAMA_URL", config.ollama_url);
  config.ollama_model = ReadEnv("OLLAMA_MODEL", config.ollama_model);
  config.data_dir = ReadEnv("DATA_DIR", config.data_dir);
  config.startup_status_file = ReadEnv("STARTUP_STATUS_FILE", "");
  config.query_expansion = ReadBool("QUERY_EXPANSION", true);
  config.auto_index = ReadBool("AUTO_INDEX", config.auto_index);
  config.index_threads =
      ReadPositive("INDEX_THREADS", config.index_threads, 64);
  config.search_workers =
      ReadPositive("SEARCH_WORKERS", config.search_workers, 64);
  config.search_task_queue =
      ReadPositive("SEARCH_TASK_QUEUE", config.search_task_queue, 10000);
  config.search_connection_queue = ReadPositive(
      "SEARCH_CONNECTION_QUEUE", config.search_connection_queue, 1000);
  return config;
}

}  // namespace search
