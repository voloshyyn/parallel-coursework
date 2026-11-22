#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <crow.h>

#include "core/thread_pool.h"
#include "services/index_service.h"
#include "services/search_pipeline.h"

namespace search {

// Options for the WebSocket search service.
struct SearchSocketOptions {
  // Workers that run the pipeline away from Crow's I/O thread.
  std::size_t workers = 4;
  // Maximum number of waiting pipeline tasks in the pool.
  std::size_t task_queue_capacity = 64;
  // Maximum number of queries queued on one connection (the running one is
  // not counted).
  std::size_t per_connection_queue = 4;
};

// Runs /ws/search work on the project's own bounded ThreadPool.
//
// Crow calls OnMessage on its I/O executor; blocking it on Ollama would stall
// every other connection served by the same worker. The service validates the
// message, queues it per connection and lets a pool worker run expansion and
// search. Events are sent through connection.send_text(), which Crow posts to
// the connection's executor.
//
// Per-connection ordering is preserved: at most one worker task per connection
// drains its queue in FIFO order. A session is marked closed in OnClose before
// Crow can destroy the connection; subsequent sends from a running task become
// no-ops, so no raw pointer to a destroyed connection is ever used.
class SearchSocketService {
 public:
  SearchSocketService(IndexService& index_service,
                      const SearchPipeline& pipeline,
                      SearchSocketOptions options = {});
  ~SearchSocketService();

  SearchSocketService(const SearchSocketService&) = delete;
  SearchSocketService& operator=(const SearchSocketService&) = delete;

  void OnOpen(crow::websocket::connection& connection);
  void OnMessage(crow::websocket::connection& connection,
                 const std::string& message);
  void OnClose(crow::websocket::connection& connection);

  // Marks every session closed and drains the pool. Safe to call repeatedly.
  void Shutdown();

 private:
  struct Session;

  std::shared_ptr<Session> FindOrCreateSession(
      crow::websocket::connection& connection);
  void RunSession(const std::shared_ptr<Session>& session);
  void SendError(Session& session, const std::string& message) const;

  IndexService& index_service_;
  const SearchPipeline& pipeline_;
  const SearchSocketOptions options_;
  ThreadPool pool_;

  mutable std::mutex sessions_mutex_;
  std::unordered_map<const void*, std::shared_ptr<Session>> sessions_;
  bool shutting_down_ = false;
};

// Registers /ws/search and forwards its events to `service`, which must
// outlive the application.
void RegisterSearchSocketRoute(crow::SimpleApp& app,
                               SearchSocketService& service);

}  // namespace search
