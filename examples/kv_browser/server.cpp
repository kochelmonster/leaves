/**
 * KV Browser Demo — Native WebSocket server
 *
 * Hosts a ReplicatingMapStorage and handles multiple browser clients
 * via WebSocket. Each client connection runs on its own thread; a
 * global mutex serializes replication access to the DB.
 *
 * Sync protocol per client request:
 *   1. Client sends text "SYNC"
 *   2. Server→Client: ReplicationSender FSM (binary LVRP)
 *   3. Server sends text "PULL"
 *   4. Client→Server: ReplicationReceiver FSM (binary LVRP)
 *   5. Server sends text "DONE"
 *   6. Notify other connected clients via text "SYNC"
 *
 * Usage: ./kv_demo_server <port> <db_path>
 */

#ifndef TESTING
#define TESTING
#endif

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "leaves/replication.hpp"
#include "leaves/replicating_mmap.hpp"

namespace beast = boost::beast;
namespace ws    = beast::websocket;
namespace net   = boost::asio;
using tcp = net::ip::tcp;
using namespace leaves;

// ── WebSocket transport adapter ─────────────────────────────────

struct BeastWsTransport : ReplicationTransport {
  ws::stream<tcp::socket>& _ws;
  explicit BeastWsTransport(ws::stream<tcp::socket>& w) : _ws(w) {}

  void send(const uint8_t* data, size_t size) override {
    _ws.binary(true);
    _ws.write(net::buffer(data, size));
  }
};

// ── Client session ──────────────────────────────────────────────

struct ClientSession {
  std::shared_ptr<ws::stream<tcp::socket>> wss;
  int id;
  std::atomic<bool> alive{true};
};

// ── Globals ─────────────────────────────────────────────────────

static std::mutex g_db_mutex;
static std::mutex g_clients_mutex;
static std::vector<std::shared_ptr<ClientSession>> g_clients;
static std::atomic<int> g_next_id{1};

static void remove_client(int id) {
  std::lock_guard<std::mutex> lock(g_clients_mutex);
  g_clients.erase(
      std::remove_if(g_clients.begin(), g_clients.end(),
                     [id](const auto& c) { return c->id == id; }),
      g_clients.end());
}

// ── Replication events (no-op for demo) ─────────────────────────

struct DemoEvents : ReplicationEvents {
  bool completed = false;
  bool errored = false;
  void on_complete(uint64_t, size_t n) override {
    completed = true;
  }
  void on_error(uint64_t, ReplicationError, const char* r) override {
    std::cerr << "[server] repl error: " << r << "\n";
    errored = true;
  }
  void on_progress(uint64_t, size_t, size_t) override {}
};

// ── Handle one client ───────────────────────────────────────────

static void handle_client(
    std::shared_ptr<ClientSession> session,
    ReplicatingMapStorage::storage_ptr storage) {
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

      if (msg != "SYNC") {
        continue;
      }

      std::cerr << "[server] client " << client_id << " sync start\n";

      // Lock DB for exclusive replication access
      std::lock_guard<std::mutex> db_lock(g_db_mutex);

      auto db = (*storage)["main"];

      // Phase 1: Server → Client (we send)
      {
        DemoEvents events;
        BeastWsTransport transport(wss);
        ReplicationSender<ReplicatingMapStorage> sender(db);
        sender.begin(&transport, &events);

        while (sender.state() == ReplicationState::ACTIVE) {
          wss.read(buf);
          auto d = buf.cdata();
          sender.on_message_received(
              static_cast<const uint8_t*>(d.data()), d.size());
          buf.consume(buf.size());
        }

        if (events.errored) {
          std::cerr << "[server] client " << client_id
                    << " send phase failed\n";
          continue;
        }
      }

      // Send "PULL" to signal client should now send
      wss.text(true);
      wss.write(net::buffer(std::string("PULL")));

      // Phase 2: Client → Server (we receive)
      {
        DemoEvents events;
        BeastWsTransport transport(wss);
        ReplicationReceiver<ReplicatingMapStorage> receiver(db);
        receiver.begin(&transport, &events);

        while (receiver.state() == ReplicationState::ACTIVE) {
          wss.read(buf);

          if (wss.got_text()) {
            // Unexpected text during receive phase — skip
            buf.consume(buf.size());
            continue;
          }

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

        if (events.errored) {
          std::cerr << "[server] client " << client_id
                    << " receive phase failed\n";
        }
      }

      // Send "DONE" to signal sync complete
      wss.text(true);
      wss.write(net::buffer(std::string("DONE")));

      std::cerr << "[server] client " << client_id << " sync done\n";
    }
  } catch (beast::system_error const& se) {
    if (se.code() != ws::error::closed) {
      std::cerr << "[server] client " << client_id
                << " error: " << se.code().message() << "\n";
    }
  } catch (std::exception const& e) {
    std::cerr << "[server] client " << client_id
              << " exception: " << e.what() << "\n";
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
  auto storage = ReplicatingMapStorage::create(path);
  // Ensure DB "main" exists
  { auto db = (*storage)["main"]; }

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
