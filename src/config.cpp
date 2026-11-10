#include "config.h"

#include <charconv>
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

}  // namespace

Config Config::FromEnvironment() {
  Config config;
  config.port = ReadPort(ReadEnv("PORT", std::to_string(config.port)));
  config.ollama_url = ReadEnv("OLLAMA_URL", config.ollama_url);
  config.ollama_model = ReadEnv("OLLAMA_MODEL", config.ollama_model);
  config.data_dir = ReadEnv("DATA_DIR", config.data_dir);
  return config;
}

}  // namespace search
