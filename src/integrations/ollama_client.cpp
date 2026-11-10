#include "integrations/ollama_client.h"

#include <httplib.h>

#include <exception>
#include <utility>

namespace search {

OllamaClient::OllamaClient(std::string base_url,
                           std::chrono::milliseconds timeout)
    : base_url_(std::move(base_url)), timeout_(timeout) {}

bool OllamaClient::IsAvailable() const {
  try {
    httplib::Client client(base_url_);
    client.set_connection_timeout(timeout_);
    client.set_read_timeout(timeout_);
    client.set_write_timeout(timeout_);

    const httplib::Result result = client.Get("/api/tags");
    return result && result->status == 200;
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace search
