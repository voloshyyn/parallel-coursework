#include <crow.h>

#include <chrono>
#include <exception>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

#include "api.h"
#include "config.h"
#include "core/search_engine.h"
#include "integrations/ollama_client.h"
#include "integrations/ollama_monitor.h"
#include "services/index_service.h"
#include "services/search_pipeline.h"
#include "services/search_socket.h"
#include "web_ui.h"

int main() {
  const search::Config config = search::Config::FromEnvironment();
  const search::OllamaClient ollama(config.ollama_url);

  // Health answers from a cached flag, so Ollama is probed in the background.
  search::OllamaMonitor monitor(
      [&ollama, &config] { return ollama.IsAvailable(config.ollama_model); },
      std::chrono::seconds(5));
  monitor.Start();

  search::IndexService index_service(config.data_dir);
  const search::SearchEngine engine(config.data_dir);
  // Query expansion is best effort: an empty term list means the search runs
  // on the original query.
  const search::SearchPipeline pipeline(
      engine, [&ollama, model = config.ollama_model,
               enabled = config.query_expansion](std::string_view query) {
        return enabled ? ollama.ExpandQuery(query, model)
                       : std::vector<std::string>{};
      });

  search::SearchSocketOptions socket_options;
  socket_options.workers = config.search_workers;
  socket_options.task_queue_capacity = config.search_task_queue;
  socket_options.per_connection_queue = config.search_connection_queue;
  search::SearchSocketService search_socket(index_service, pipeline,
                                            socket_options);

  crow::SimpleApp app;
  search::RegisterUiRoutes(app);
  search::RegisterHealthRoute(app, monitor, &index_service,
                              config.startup_status_file);
  search::RegisterSearchRoutes(app, index_service, pipeline);
  search::RegisterSearchSocketRoute(app, search_socket);

  // HTTP is ready before the index is: a composed deployment builds the index
  // in the background and /api/ready reports `index_state: "building"` until
  // search can serve requests.
  std::jthread indexer;
  if (config.auto_index) {
    indexer = std::jthread([&index_service, threads = config.index_threads] {
      try {
        const auto stats = index_service.Build(threads);
        if (stats) {
          std::cout << "index built: " << stats->documents_indexed
                    << " documents in " << stats->build_time_ms << " ms\n";
        }
      } catch (const std::exception& error) {
        std::cerr << "automatic index build failed: " << error.what() << '\n';
      }
    });
  }

  app.loglevel(crow::LogLevel::Warning);
  std::cout << "search_server listening on 0.0.0.0:" << config.port
            << std::endl;
  app.bindaddr("0.0.0.0").port(config.port).multithreaded().run();

  monitor.Stop();
  if (indexer.joinable()) {
    indexer.join();
  }
  // Stop background searches before the application object goes away.
  search_socket.Shutdown();
  return 0;
}
