#pragma once

#include <string>

#include <crow.h>

#include "integrations/ollama_monitor.h"

namespace search {

// Body of GET /api/health. Mirrors the contract in docs/api.md.
std::string HealthJson(bool ollama_available);

// Registers GET /api/health on the application. The handler only reads the
// cached Ollama state and always answers 200.
void RegisterHealthRoute(crow::SimpleApp& app, const OllamaMonitor& monitor);

}  // namespace search
