/**
 * KV Browser Demo — Native WebSocket server
 *
 * Hosts a MapStorage and handles multiple browser clients
 * via WebSocket. Each client connection runs on its own thread; a
 * global mutex serializes replication access to the DB.
 *
 * Sync protocol per client request:
 *   "PUSH": Client has local changes, sends them to server
 * (ReplicationReceiver). Server broadcasts "SYNC" notification to other
 * clients. "PULL": Client wants server's data, server sends to client
 * (ReplicationSender). Server always replies with "DONE" when the binary
 * transfer completes. Server broadcasts "SYNC" text to all clients after a
 * successful PUSH, telling them to pull the latest state.
 *
 * Usage: ./kv_demo_server <port> <db_path>
 */

#ifndef TESTING
#define TESTING
#endif

#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "leaves/mmap.hpp"
#include "leaves/replication.hpp"

namespace beast = boost::beast;
namespace ws = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using namespace leaves;

static constexpr auto SYNC_DEBOUNCE = std::chrono::milliseconds(100);

thread_local int g_commit_origin_client_id = -1;

struct ServerCommitScope {
  explicit ServerCommitScope(int client_id) {
    g_commit_origin_client_id = client_id;
  }

  ~ServerCommitScope() { g_commit_origin_client_id = -1; }
};

// ── WebSocket transport adapter ─────────────────────────────────

struct BeastWsTransport : ReplicationTransport {
  std::mutex& _write_mutex;
  ws::stream<tcp::socket>& _ws;
  BeastWsTransport(ws::stream<tcp::socket>& w, std::mutex& write_mutex)
      : _write_mutex(write_mutex), _ws(w) {}

  void send(const uint8_t* data, size_t size) override {
    std::lock_guard<std::mutex> lock(_write_mutex);
    _ws.binary(true);
    _ws.write(net::buffer(data, size));
  }
};

// ── Client session ──────────────────────────────────────────────

struct ClientSession {
  std::shared_ptr<ws::stream<tcp::socket>> wss;
  int id;
  std::atomic<bool> alive{true};
  std::mutex write_mutex;
};

// ── Globals ─────────────────────────────────────────────────────

static std::mutex g_db_mutex;
static std::mutex g_clients_mutex;
static std::vector<std::shared_ptr<ClientSession>> g_clients;
static std::atomic<int> g_next_id{1};

static void broadcast_sync_notification(const std::set<int>& excluded_clients) {
  std::vector<std::shared_ptr<ClientSession>> clients;
  {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    clients = g_clients;
  }

  std::cerr << "[server] broadcast SYNC to " << clients.size()
            << " clients (excluding " << excluded_clients.size() << ")\n";

  for (const auto& session : clients) {
    if (!session || !session->alive.load()) continue;
    if (excluded_clients.count(session->id)) continue;

    try {
      std::lock_guard<std::mutex> lock(session->write_mutex);
      session->wss->text(true);
      session->wss->write(net::buffer(std::string("SYNC")));
    } catch (const std::exception&) {
      session->alive = false;
    }
  }
}

struct ServerSyncNotifier {
  std::mutex mutex;
  uint64_t pending_job_id = 0;
  tid_t _last_txn_id{};
  std::set<int> excluded_clients;

  template <typename Pool>
  void schedule(Pool& pool, tid_t txn_id_snapshot, int origin_client_id) {
    std::lock_guard<std::mutex> lock(mutex);
    _last_txn_id = txn_id_snapshot;
    if (origin_client_id > 0) {
      excluded_clients.insert(origin_client_id);
    }
    if (pending_job_id) {
      pool.cancel_job(pending_job_id);
    }
    pending_job_id =
        pool.schedule_after(SYNC_DEBOUNCE, [this, txn_id_snapshot]() {
          std::set<int> excluded;
          {
            std::lock_guard<std::mutex> lock(mutex);
            if (txn_id_snapshot != _last_txn_id) {
              std::cerr << "[server] sync debounce: skipped (txn changed)\n";
              return;
            }
            pending_job_id = 0;
            excluded.swap(excluded_clients);
          }
          broadcast_sync_notification(excluded);
        });
  }
};

static ServerSyncNotifier g_sync_notifier;

struct ServerAspect : public DefaultAspect {
  template <typename DB, typename Ctx>
  void on_commit(DB& db, TransactionOrigin, Ctx&) {
    if (g_commit_origin_client_id <= 0) {
      std::cerr << "[server] on_commit skipped (origin="
                << g_commit_origin_client_id << ")\n";
      return;
    }
    std::cerr << "[server] on_commit triggered by client "
              << g_commit_origin_client_id << "\n";
    g_sync_notifier.schedule(db._storage, db.txn()->txn_id,
                             g_commit_origin_client_id);
  }
};

struct ServerReplicatingMapTraits : public _MemoryMapTraits {
  using Aspect = ServerAspect;
};

using ServerStorage = MapStorage_<ServerReplicatingMapTraits>;

static void remove_client(int id) {
  std::lock_guard<std::mutex> lock(g_clients_mutex);
  g_clients.erase(std::remove_if(g_clients.begin(), g_clients.end(),
                                 [id](const auto& c) { return c->id == id; }),
                  g_clients.end());
}

// ── Replication events (no-op for demo) ─────────────────────────

struct DemoEvents : ReplicationEvents {
  bool completed = false;
  bool errored = false;
  void on_complete(uint64_t session_id, size_t n) override {
    std::cerr << "[server] on_complete session=" << session_id << " n=" << n
              << "\n";
    completed = true;
  }
  void on_error(uint64_t session_id, ReplicationError err,
                const char* r) override {
    std::cerr << "[server] on_error session=" << session_id
              << " err=" << static_cast<int>(err) << " msg=" << r << "\n";
    errored = true;
  }
  void on_progress(uint64_t, size_t, size_t) override {}
};

// ── Handle one client ───────────────────────────────────────────

static void handle_client(std::shared_ptr<ClientSession> session,
                          ServerStorage::storage_ptr storage) {
  auto& wss = *session->wss;
  int client_id = session->id;

  std::cerr << "[server] client " << client_id << " connected\n";

  try {
    beast::flat_buffer buf;

    while (true) {
      // Wait for a text message from the client
      wss.read(buf);

      if (!wss.got_text()) {
        buf.consume(buf.size());
        continue;
      }

      auto msg = beast::buffers_to_string(buf.cdata());
      buf.consume(buf.size());

      if (msg == "PUSH") {
        // Client has local changes, pushes them to us
        std::cerr << "[server] client " << client_id << " push start\n";
        std::lock_guard<std::mutex> db_lock(g_db_mutex);
        auto db = storage->open<ServerStorage::ReplicationDB>("main");

        ServerCommitScope commit_scope(client_id);
        DemoEvents events;
        BeastWsTransport transport(wss, session->write_mutex);
        ReplicationReceiver<ServerStorage> receiver(db);
        receiver.begin(&transport, &events);

        while (receiver.state() == ReplicationState::ACTIVE) {
          wss.read(buf);
          auto d = buf.cdata();
          auto& rb = receiver.receive_buffer();
          auto* src = static_cast<const uint8_t*>(d.data());
          size_t todo = d.size();
          size_t off = 0;

          while (off < todo) {
            size_t chunk = std::min(todo - off, rb.available());
            if (chunk == 0) break;
            std::memcpy(rb.write_ptr(), src + off, chunk);
            rb.advance(chunk);
            off += chunk;
            receiver.on_data_received();
          }
          buf.consume(buf.size());
        }

        if (!events.errored) {
          std::cerr << "[server] client " << client_id
                    << " push done: nodes=" << receiver.nodes_transferred()
                    << " bytes=" << receiver.bytes_transferred() << "\n";
          // Broadcast SYNC notification to other clients
          g_sync_notifier.schedule(storage->thread_pool(), db.txn()->txn_id,
                                   client_id);
        } else {
          std::cerr << "[server] client " << client_id << " push failed\n";
        }

        // Tell client we're done
        {
          std::lock_guard<std::mutex> lock(session->write_mutex);
          wss.text(true);
          wss.write(net::buffer(std::string("DONE")));
        }

      } else if (msg == "PULL") {
        // Client wants our data, we send to them
        std::cerr << "[server] client " << client_id << " pull start\n";
        std::lock_guard<std::mutex> db_lock(g_db_mutex);
        auto db = storage->open<ServerStorage::ReplicationDB>("main");

        DemoEvents events;
        BeastWsTransport transport(wss, session->write_mutex);
        ReplicationSender<ServerStorage> sender(db);
        sender.begin(&transport, &events);

        while (sender.state() == ReplicationState::ACTIVE) {
          wss.read(buf);
          auto d = buf.cdata();
          sender.on_message_received(static_cast<const uint8_t*>(d.data()),
                                     d.size());
          buf.consume(buf.size());
        }

        if (!events.errored) {
          std::cerr << "[server] client " << client_id
                    << " pull done: nodes=" << sender.nodes_transferred()
                    << " bytes=" << sender.bytes_transferred() << "\n";
        } else {
          std::cerr << "[server] client " << client_id << " pull failed\n";
        }

        // Tell client we're done
        {
          std::lock_guard<std::mutex> lock(session->write_mutex);
          wss.text(true);
          wss.write(net::buffer(std::string("DONE")));
        }

      } else {
        std::cerr << "[server] client " << client_id
                  << " ignoring text: " << msg << "\n";
        continue;
      }

      std::cerr << "[server] client " << client_id << " done\n";
    }
  } catch (beast::system_error const& se) {
    if (se.code() != ws::error::closed) {
      std::cerr << "[server] client " << client_id
                << " error: " << se.code().message() << "\n";
    }
  } catch (std::exception const& e) {
    std::cerr << "[server] client " << client_id << " exception: " << e.what()
              << "\n";
  }

  session->alive = false;
  remove_client(client_id);
  std::cerr << "[server] client " << client_id << " disconnected\n";
}

// ── Main ────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " <port> <db_path>\n";
    return 1;
  }

  uint16_t port = static_cast<uint16_t>(std::atoi(argv[1]));
  const char* path = argv[2];

  // Create storage
  auto storage = ServerStorage::create(path);
  // Ensure DB "main" exists
  {
    auto db = storage->open<ServerStorage::ReplicationDB>("main");
  }

  std::cerr << "[server] storage opened: " << path << "\n";

  // Set up acceptor
  net::io_context ioc{1};
  tcp::acceptor acceptor{ioc, tcp::endpoint{tcp::v4(), port}};

  // Signal readiness
  std::cout << "READY " << port << std::endl;
  std::cerr << "[server] listening on port " << port << "\n";

  // Accept loop
  std::vector<std::thread> threads;

  while (true) {
    tcp::socket socket{ioc};
    acceptor.accept(socket);

    auto wss = std::make_shared<ws::stream<tcp::socket>>(std::move(socket));
    wss->accept();

    auto session = std::make_shared<ClientSession>();
    session->wss = wss;
    session->id = g_next_id.fetch_add(1);

    {
      std::lock_guard<std::mutex> lock(g_clients_mutex);
      g_clients.push_back(session);
    }

    threads.emplace_back(handle_client, session, storage);
    threads.back().detach();
  }

  return 0;
}