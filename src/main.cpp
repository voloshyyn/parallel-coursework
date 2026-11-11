#include <crow.h>

#include <chrono>
#include <iostream>
#include <string_view>

#include "api.h"
#include "config.h"
#include "core/search_engine.h"
#include "integrations/ollama_client.h"
#include "integrations/ollama_monitor.h"
#include "services/index_service.h"
#include "services/search_pipeline.h"

int main() {
  const search::Config config = search::Config::FromEnvironment();
  const search::OllamaClient ollama(config.ollama_url);

  // Health answers from a cached flag, so Ollama is probed in the background.
  search::OllamaMonitor monitor([&ollama] { return ollama.IsAvailable(); },
                                std::chrono::seconds(5));
  monitor.Start();

  search::IndexService index_service(config.data_dir);
  const search::SearchEngine engine(config.data_dir);
  // Query expansion is best effort: an empty term list means the search runs
  // on the original query.
  const search::SearchPipeline pipeline(
      engine, [&ollama, model = config.ollama_model](std::string_view query) {
        return ollama.ExpandQuery(query, model);
      });

  crow::SimpleApp app;
  search::RegisterHealthRoute(app, monitor);
  search::RegisterSearchRoutes(app, index_service, pipeline);
  search::RegisterSearchSocketRoute(app, index_service, pipeline);

  app.loglevel(crow::LogLevel::Warning);
  std::cout << "search_server listening on 0.0.0.0:" << config.port
            << std::endl;
  app.bindaddr("0.0.0.0").port(config.port).multithreaded().run();
  monitor.Stop();
  return 0;
}
