#pragma once

#include <crow.h>

namespace search {

// Registers GET /, /app.js and /style.css: the Web UI is served by the same
// process as the API, so `docker compose up` is still the only command needed
// (docs/ui.md).
void RegisterUiRoutes(crow::SimpleApp& app);

}  // namespace search
