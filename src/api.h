#pragma once

#include <string>

#include <crow.h>

#include "integrations/ollama_monitor.h"
#include "services/index_service.h"
#include "services/search_pipeline.h"

namespace search {

// Body of GET /api/health. Mirrors the contract in docs/api.md.
std::string HealthJson(bool ollama_available);

// Registers GET /api/health on the application. The handler only reads the
// cached Ollama state and always answers 200.
void RegisterHealthRoute(crow::SimpleApp& app, const OllamaMonitor& monitor);

// Registers POST /api/index, POST /api/search and GET /api/stats. Handlers only
// validate the request and delegate to IndexService and SearchPipeline.
void RegisterSearchRoutes(crow::SimpleApp& app, IndexService& index_service,
                          const SearchPipeline& pipeline);

}  // namespace search
