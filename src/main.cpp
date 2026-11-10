#include <crow.h>

#include <chrono>
#include <iostream>

#include "api.h"
#include "config.h"
#include "integrations/ollama_client.h"
#include "integrations/ollama_monitor.h"

int main() {
  const search::Config config = search::Config::FromEnvironment();
  const search::OllamaClient ollama(config.ollama_url);

  // Health answers from a cached flag, so Ollama is probed in the background.
  search::OllamaMonitor monitor([&ollama] { return ollama.IsAvailable(); },
                                std::chrono::seconds(5));
  monitor.Start();

  crow::SimpleApp app;
  search::RegisterHealthRoute(app, monitor);

  app.loglevel(crow::LogLevel::Warning);
  std::cout << "search_server listening on 0.0.0.0:" << config.port
            << std::endl;
  app.bindaddr("0.0.0.0").port(config.port).multithreaded().run();
  monitor.Stop();
  return 0;
}
