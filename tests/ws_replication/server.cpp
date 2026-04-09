/**
 * WebSocket replication server (native binary)
 *
 * Hosts a MapStorage with test data, serves one WebSocket
 * connection on localhost:<port>, runs the LVRP sender FSM over it.
 *
 * Usage: ./test_ws_replication_server <port> <db_path>
 */

#ifndef TESTING
#define TESTING
#endif

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "leaves/replication.hpp"
#include "leaves/mmap.hpp"
#include "leaves/intern/replication/_replication_db.hpp"

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

// ── Main ────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " <port> <db_path>\n";
    return 1;
  }

  uint16_t port    = static_cast<uint16_t>(std::atoi(argv[1]));
  const char* path = argv[2];

  std::filesystem::remove(path);

  bool completed = false;

  {
    // Create source storage and populate with test data
    auto src_storage = MapStorage::create(path);
    auto src_db = src_storage->open<_ReplicationDB>("testdb");
    {
      auto c = src_db.cursor();
      c.start_transaction();
      c.find(Slice("hello"));
      c.value(Slice("world"));
      c.find(Slice("foo"));
      c.value(Slice("bar"));
      c.find(Slice("count"));
      c.value(Slice("12345"));
      c.commit();
    }
    std::cerr << "[server] data seeded\n";

    // Accept one connection
    net::io_context ioc{1};
    tcp::acceptor acceptor{ioc, tcp::endpoint{tcp::v4(), port}};

    // Signal readiness to orchestrator via stdout
    std::cout << "READY " << port << std::endl;

    tcp::socket socket{ioc};
    acceptor.accept(socket);
    std::cerr << "[server] client connected\n";

    // WebSocket handshake
    ws::stream<tcp::socket> wss{std::move(socket)};
    wss.accept();

    // Replication sender
    BeastWsTransport transport(wss);

    struct Events : ReplicationEvents {
      bool& _completed;
      bool errored   = false;
      Events(bool& c) : _completed(c) {}
      void on_complete(uint64_t, size_t n) override {
        std::cerr << "[server] complete, " << n << " nodes\n";
        _completed = true;
      }
      void on_error(uint64_t, ReplicationError, const char* r) override {
        std::cerr << "[server] error: " << r << "\n";
        errored = true;
      }
      void on_progress(uint64_t, size_t, size_t) override {}
    } events(completed);

    ReplicationSender<MapStorage> sender(src_db);
    sender.begin(&transport, &events);

    // Protocol loop — read receiver responses
    beast::flat_buffer buf;
    while (sender.state() == ReplicationState::ACTIVE) {
      wss.read(buf);
      auto d = buf.cdata();
      sender.on_message_received(
          static_cast<const uint8_t*>(d.data()), d.size());
      buf.consume(buf.size());
    }

    // Clean shutdown
    beast::error_code ec;
    wss.close(ws::close_code::normal, ec);
  }  // storage destroyed here

  std::filesystem::remove(path);

  std::cerr << "[server] exit " << (completed ? 0 : 1) << "\n";
  return completed ? 0 : 1;
}
