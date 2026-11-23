#include "services/search_socket.h"

#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace search {
namespace {

using Json = nlohmann::json;

Json TermsJson(const std::vector<std::string>& terms) {
  Json array = Json::array();
  for (const std::string& term : terms) {
    array.push_back(term);
  }
  return array;
}

Json HitsJson(const std::vector<SearchResult>& results) {
  Json hits = Json::array();
  for (const SearchResult& result : results) {
    hits.push_back(Json{{"doc_id", result.doc_id},
                        {"score", result.score},
                        {"snippet", result.snippet}});
  }
  return hits;
}

// Parses the documented request shape. Returns the query, or fills `error`.
bool ParseQuery(const std::string& message, std::string& query,
                std::string& error) {
  const Json body = Json::parse(message, nullptr, false);
  if (body.is_discarded() || !body.is_object()) {
    error = "invalid JSON body";
    return false;
  }
  if (!body.contains("query") || !body.at("query").is_string() ||
      body.at("query").get<std::string>().empty()) {
    error = "query must be a non-empty string";
    return false;
  }
  query = body.at("query").get<std::string>();
  return true;
}

}  // namespace

// One connection: its queue, its running flag and the send guard.
struct SearchSocketService::Session {
  struct Request {
    std::string query;
    std::string error;
  };
  explicit Session(crow::websocket::connection& connection)
      : connection(&connection) {}

  // Serialises send with close. OnClose sets `closed` and clears `connection`
  // under the same mutex before Crow may destroy it, so a background task
  // either sends while the connection is alive or returns without touching it.
  void Send(const Json& message) {
    std::lock_guard<std::mutex> lock(mutex);
    if (closed || connection == nullptr ||
        (aborted && message.at("stage") != "error")) {
      return;
    }
    connection->send_text(message.dump());
  }

  crow::websocket::connection* connection;
  mutable std::mutex mutex;
  std::deque<Request> pending;
  bool running = false;
  bool closed = false;
  bool aborted = false;
};

SearchSocketService::SearchSocketService(IndexService& index_service,
                                         const SearchPipeline& pipeline,
                                         SearchSocketOptions options)
    : index_service_(index_service),
      pipeline_(pipeline),
      options_(options),
      pool_(options.workers, options.task_queue_capacity) {}

SearchSocketService::~SearchSocketService() { Shutdown(); }

std::shared_ptr<SearchSocketService::Session>
SearchSocketService::FindOrCreateSession(
    crow::websocket::connection& connection) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  const auto found = sessions_.find(&connection);
  if (found != sessions_.end()) {
    return found->second;
  }
  auto session = std::make_shared<Session>(connection);
  sessions_.emplace(&connection, session);
  return session;
}

void SearchSocketService::OnOpen(crow::websocket::connection& connection) {
  FindOrCreateSession(connection);
}

void SearchSocketService::SendError(Session& session,
                                    const std::string& message) const {
  session.Send(Json{{"stage", "error"}, {"data", Json{{"message", message}}}});
}

void SearchSocketService::OnMessage(crow::websocket::connection& connection,
                                    const std::string& message) {
  const std::shared_ptr<Session> session = FindOrCreateSession(connection);

  // Read the flag under the map mutex, but send after releasing it: a slow
  // socket must not block other connections' lookups.
  bool shutting_down = false;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    shutting_down = shutting_down_;
  }
  if (shutting_down) {
    SendError(*session, "server is shutting down");
    return;
  }

  std::string query;
  std::string error;
  ParseQuery(message, query, error);

  // Same rules as POST /api/search, checked here so the error is immediate.
  if (error.empty()) {
    if (index_service_.State() == IndexState::kBuilding) {
      error = "index is building";
    } else if (!index_service_.Snapshot().index) {
      error = "index not built";
    }
  }

  bool submit = false;
  bool overflow = false;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->closed) {
      return;
    }
    if (session->aborted ||
        session->pending.size() >= options_.per_connection_queue) {
      overflow = true;
      // There are no request IDs in the fixed protocol. Once overloaded,
      // abort this batch so error is never followed by stale results/done.
      session->aborted = true;
      session->pending.clear();
    } else {
      session->pending.push_back({std::move(query), std::move(error)});
      if (!session->running) {
        session->running = true;
        submit = true;
      }
    }
  }
  if (overflow) {
    SendError(*session, "server is busy");
    return;
  }
  if (!submit) {
    return;
  }

  const bool accepted = pool_.Submit([this, session] { RunSession(session); });
  if (!accepted) {
    // The pool refused the task (bounded queue or shutdown). Nothing can run
    // for this connection now, so drop its queued queries and report once.
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      session->running = false;
      session->pending.clear();
    }
    SendError(*session, "server is busy");
  }
}

void SearchSocketService::RunSession(const std::shared_ptr<Session>& session) {
  for (;;) {
    Session::Request request;
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      if (session->closed) {
        session->running = false;
        return;
      }
      if (session->aborted || session->pending.empty()) {
        session->running = false;
        session->aborted = false;
        return;
      }
      request = std::move(session->pending.front());
      session->pending.pop_front();
    }

    if (!request.error.empty()) {
      SendError(*session, request.error);
      continue;
    }

    // A rebuild may have started between two messages of one connection.
    if (index_service_.State() == IndexState::kBuilding) {
      SendError(*session, "index is building");
      continue;
    }
    const PublishedIndex snapshot = index_service_.Snapshot();
    if (!snapshot.index) {
      SendError(*session, "index not built");
      continue;
    }

    try {
      PipelineObserver observer;
      observer.on_expansion = [session](std::string_view original,
                                        const std::vector<std::string>& terms) {
        session->Send(
            Json{{"stage", "query_expansion"},
                 {"data", Json{{"original", original},
                               {"expanded_terms", TermsJson(terms)}}}});
      };
      observer.on_results = [session](const SearchOutcome& outcome) {
        session->Send(
            Json{{"stage", "keyword_search"},
                 {"data", Json{{"results", HitsJson(outcome.results)},
                               {"time_ms", outcome.search_time_ms}}}});
      };

      pipeline_.Run(*snapshot.index, request.query, observer);
      session->Send(Json{{"stage", "done"}});
    } catch (const std::exception& exception) {
      SendError(*session, exception.what());
    } catch (...) {
      SendError(*session, "internal error");
    }
  }
}

void SearchSocketService::OnClose(crow::websocket::connection& connection) {
  std::shared_ptr<Session> session;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    const auto found = sessions_.find(&connection);
    if (found == sessions_.end()) {
      return;
    }
    session = std::move(found->second);
    sessions_.erase(found);
  }
  {
    // Wait for an in-flight send to finish, then forbid all later ones. The
    // close handler runs before Crow removes the connection, so once this lock
    // is released the background task can no longer touch it.
    std::lock_guard<std::mutex> lock(session->mutex);
    session->closed = true;
    session->connection = nullptr;
    session->pending.clear();
  }
}

void SearchSocketService::Shutdown() {
  std::vector<std::shared_ptr<Session>> sessions;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    shutting_down_ = true;
    for (auto& [connection, session] : sessions_) {
      sessions.push_back(std::move(session));
    }
    sessions_.clear();
  }
  for (const std::shared_ptr<Session>& session : sessions) {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->closed = true;
    session->connection = nullptr;
    session->pending.clear();
  }
  pool_.Shutdown();
}

void RegisterSearchSocketRoute(crow::SimpleApp& app,
                               SearchSocketService& service) {
  CROW_WEBSOCKET_ROUTE(app, "/ws/search")
      .onopen([&service](crow::websocket::connection& connection) {
        service.OnOpen(connection);
      })
      .onmessage([&service](crow::websocket::connection& connection,
                            const std::string& message, bool /*is_binary*/) {
        service.OnMessage(connection, message);
      })
      .onclose([&service](crow::websocket::connection& connection,
                          const std::string& /*message*/,
                          std::uint16_t /*status_code*/) {
        service.OnClose(connection);
      });
}

}  // namespace search
