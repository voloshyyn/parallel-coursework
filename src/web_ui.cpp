#include "web_ui.h"

#include <crow.h>

#include <string>
#include <string_view>

#include "web_assets.h"

namespace search {
namespace {

// Assets are small and embedded, so the browser needs no cache; this also keeps
// a reload after a rebuild showing the current UI.
crow::response AssetResponse(std::string_view body, const char* content_type) {
  crow::response response(200, std::string(body));
  response.set_header("Content-Type", content_type);
  response.set_header("Cache-Control", "no-cache");
  return response;
}

}  // namespace

void RegisterUiRoutes(crow::SimpleApp& app) {
  // Vue is loaded from a CDN; the page calls /api/* and /ws/search on the same
  // origin, so no CORS headers are involved.
  CROW_ROUTE(app, "/")([] {
    return AssetResponse(web::kIndexHtml, "text/html; charset=utf-8");
  });
  CROW_ROUTE(app, "/app.js")([] {
    return AssetResponse(web::kAppJs, "application/javascript; charset=utf-8");
  });
  CROW_ROUTE(app, "/style.css")([] {
    return AssetResponse(web::kStyleCss, "text/css; charset=utf-8");
  });
}

}  // namespace search
