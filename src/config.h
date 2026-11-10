#pragma once

#include <string>

namespace search {

// Runtime configuration read from environment variables.
struct Config {
  int port = 8080;
  std::string ollama_url = "http://localhost:11434";
  std::string ollama_model = "qwen2.5:1.5b";
  std::string data_dir = "/data/docs";

  static Config FromEnvironment();
};

}  // namespace search
