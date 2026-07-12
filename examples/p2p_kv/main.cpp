/**
 * P2P Key-Value Store — Peer-to-peer replication over TCP with
 * UDP multicast discovery.
 *
 * Each instance is a full peer (no central server).  Peers discover
 * each other via UDP multicast, establish TCP connections, and
 * continuously synchronise their databases using LVRP replication.
 *
 * Terminal commands:
 *   add <key> <value>   Insert or update a key (stores UTC timestamp)
 *   get <key>           Read payload and UTC timestamp
 *   del <key>           Remove a key
 *   list                List all keys (payload + UTC timestamp)
 *   peers               Show connected peers
 *   help                This help
 *   quit                Exit
 *
 * Usage: ./p2p_kv --port <tcp_port> --db <db_path>
 */

#ifndef TESTING
#define TESTING
#endif

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "leaves/replication.hpp"
#include "leaves/mmap.hpp"

namespace net   = boost::asio;
using tcp       = net::ip::tcp;
using udp       = net::ip::udp;
using namespace leaves;

// ── Constants ──────────────────────────────────────────────────────────────

static constexpr uint16_t MULTICAST_PORT    = 9876;
static const auto         MULTICAST_ADDR    = net::ip::make_address("239.255.0.1");
static constexpr auto     ANNOUNCE_INTERVAL = std::chrono::seconds(30);
static constexpr auto     SYNC_DEBOUNCE     = std::chrono::milliseconds(100);

// TCP frame header: [uint32_t type][uint32_t length][payload]
//   type=0  → binary LVRP message payload
//   type=1  → text  control message
static constexpr uint32_t FRAME_BINARY      = 0;
static constexpr uint32_t FRAME_TEXT        = 1;
static constexpr size_t   FRAME_HDR         = 8;  // 4+4 header
static constexpr auto     SYNC_IDLE_TIMEOUT = std::chrono::seconds(10);

// ── Forward declarations ──────────────────────────────────────────────────

class PeerSession;
struct P2pMapTraits;
struct P2pSyncNotifier;
using P2pStorage = MapStorage_<P2pMapTraits>;
static void connect_to_peer_async(const std::string& host, uint16_t port);

// ── Globals ────────────────────────────────────────────────────────────────

static net::io_context*                     g_io   = nullptr;
static std::mutex                           g_db_mutex;
static std::mutex                           g_peers_mutex;
static std::vector<std::shared_ptr<PeerSession>> g_peers;
static std::shared_ptr<P2pStorage>          g_storage;
static P2pSyncNotifier*                     g_notifier = nullptr;
static std::atomic<int>                     g_next_manual_peer_id{10000};

// Thread-local commit origin.  Set to >0 before a remote-triggered commit so
// the Aspect knows which peer originated the change (and avoids echoing SYNC
// back to that peer).  Set to -1 for local user-originated commits.
thread_local int g_commit_origin_peer_id = -1;

// ── Value encoding helpers ────────────────────────────────────────────────

// Stored value format used by this demo: <utc_epoch_ms>|<payload>
static uint64_t utc_epoch_ms_now() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());
}

static bool decode_timestamped_value(const Slice& encoded, uint64_t* out_ts,
                                     Slice* out_payload = nullptr) {
  const char* data = encoded.data();
  size_t size = encoded.size();
  size_t sep = size;

  for (size_t i = 0; i < size; ++i) {
    if (data[i] == '|') {
      sep = i;
      break;
    }
  }

  if (sep == 0 || sep == size) {
    return false;
  }

  uint64_t ts = 0;
  for (size_t i = 0; i < sep; ++i) {
    unsigned char ch = static_cast<unsigned char>(data[i]);
    if (ch < '0' || ch > '9') {
      return false;
    }
    uint64_t digit = static_cast<uint64_t>(ch - '0');
    if (ts > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
      return false;
    }
    ts = ts * 10 + digit;
  }

  if (out_ts) {
    *out_ts = ts;
  }
  if (out_payload) {
    *out_payload = Slice(data + sep + 1, size - (sep + 1));
  }
  return true;
}

static std::string encode_timestamped_value(uint64_t ts,
                                            const std::string& payload) {
  return std::to_string(ts) + "|" + payload;
}

static std::string format_value_for_display(const Slice& encoded) {
  uint64_t ts = 0;
  Slice payload;
  if (decode_timestamped_value(encoded, &ts, &payload)) {
    return payload.string() + " (utc_ms=" + std::to_string(ts) + ")";
  }
  return encoded.string() + " (raw)";
}

// ── Scoped commit origin ───────────────────────────────────────────────────

struct PeerCommitScope {
  explicit PeerCommitScope(int pid) { g_commit_origin_peer_id = pid; }
  ~PeerCommitScope()                { g_commit_origin_peer_id = -1; }
};

// ── TCP frame ──────────────────────────────────────────────────────────────

struct Frame {
  uint32_t type = 0;
  uint32_t len  = 0;
  std::vector<char> payload;

  size_t wire_size() const { return FRAME_HDR + len; }

  std::vector<char> encode() const {
    std::vector<char> buf(wire_size());
    uint32_t ntype = htonl(type);
    uint32_t nlen  = htonl(len);
    memcpy(buf.data(),          &ntype, 4);
    memcpy(buf.data() + 4,      &nlen,  4);
    if (len) memcpy(buf.data() + 8, payload.data(), len);
    return buf;
  }

  static bool decode(const char* data, size_t sz, Frame& out) {
    if (sz < FRAME_HDR) return false;
    memcpy(&out.type, data, 4); out.type = ntohl(out.type);
    memcpy(&out.len,  data + 4, 4); out.len = ntohl(out.len);
    if (sz < FRAME_HDR + out.len) return false;
    if (out.len)
      out.payload.assign(data + FRAME_HDR, data + FRAME_HDR + out.len);
    else
      out.payload.clear();
    return true;
  }
};

// ── TCP transport adapter (implements ReplicationTransport) ──────────────

struct TcpTransport : ReplicationTransport {
  tcp::socket& socket;

  explicit TcpTransport(tcp::socket& s) : socket(s) {}

  void send(const uint8_t* data, size_t size) override {
    Frame f;
    f.type = FRAME_BINARY;
    f.len  = static_cast<uint32_t>(size);
    f.payload.assign(reinterpret_cast<const char*>(data),
                     reinterpret_cast<const char*>(data) + size);
    auto wire = f.encode();
    boost::asio::write(socket, boost::asio::buffer(wire));
  }
};

// ── Event tracker ──────────────────────────────────────────────────────────

struct SyncEvents : ReplicationEvents {
  bool   completed = false;
  bool   errored   = false;
  std::string msg;

  void on_complete(uint64_t, size_t) override { completed = true; }
  void on_error(uint64_t, ReplicationError, const char* r) override {
    errored = true;
    msg = r ? r : "(unknown)";
  }
  void on_progress(uint64_t, size_t, size_t) override {}
};

// ── Debounced sync notifier ────────────────────────────────────────────────

static void broadcast_sync_peers(const std::set<int>& excluded);

struct P2pSyncNotifier {
  net::steady_timer timer_;
  std::mutex        mutex_;
  uint64_t          pending_id_ = 0;
  tid_t             last_txn_id_{};
  std::set<int>     excluded_peers_;

  explicit P2pSyncNotifier(net::io_context& io) : timer_(io) {}

  template <typename DB>
  void schedule(DB& db, int origin_peer_id) {
    auto txn_snap = db.txn()->txn_id;
    ++pending_id_;
    if (pending_id_ == 0) ++pending_id_;
    auto this_id = pending_id_;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      last_txn_id_ = txn_snap;
      if (origin_peer_id > 0)
        excluded_peers_.insert(origin_peer_id);
    }
    timer_.expires_after(SYNC_DEBOUNCE);
    timer_.async_wait([this_id, txn_snap, this](boost::system::error_code ec) {
      if (ec) return;
      std::set<int> excluded;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_id_ != this_id) return;
        if (txn_snap != last_txn_id_) return;
        excluded.swap(excluded_peers_);
      }
      broadcast_sync_peers(excluded);
    });
  }
};

// ── Aspect ─────────────────────────────────────────────────────────────────

struct P2pAspect : public DefaultAspect {
  // Last-write-wins during replication merge based on UTC timestamp in value.
  // Value format: <utc_epoch_ms>|<payload>
  bool may_merge_overwrite(const Slice&, const Slice& dst, bool,
                           const Slice& src, bool, CursorContext&) {
    uint64_t dst_ts = 0;
    uint64_t src_ts = 0;
    bool dst_ok = decode_timestamped_value(dst, &dst_ts);
    bool src_ok = decode_timestamped_value(src, &src_ts);

    if (dst_ok && src_ok) {
      // Younger (larger UTC epoch ms) value wins.
      return src_ts > dst_ts;
    }
    if (dst_ok != src_ok) {
      // Validly encoded timestamped value wins over malformed value.
      return src_ok;
    }

    // Legacy fallback keeps previous behaviour when neither side is encoded.
    return true;
  }

  template <typename DB>
  void on_commit(DB& db, TransactionOrigin, CursorContext&) {
    int origin = g_commit_origin_peer_id;
    if (g_notifier) g_notifier->schedule(db, origin);
  }
};

struct P2pMapTraits : public _MemoryMapTraits {
  using Aspect = P2pAspect;
};

// ── PeerSession ───────────────────────────────────────────────────────────

class PeerSession : public std::enable_shared_from_this<PeerSession> {
public:
  tcp::socket          socket_;
  int                  id_;
  std::atomic<bool>    alive_{true};
  std::string          remote_host_;
  uint16_t             remote_port_ = 0;

  PeerSession(net::io_context& io, int id)
      : socket_(io), id_(id) {}

  ~PeerSession() { close(); }

  void close() {
    alive_ = false;
    boost::system::error_code ec;
    socket_.close(ec);
  }

  std::string label() const {
    return remote_host_ + ":" + std::to_string(remote_port_);
  }

  // ── Async read loop ──────────────────────────────────────────────────
  void start_read_loop() {
    auto self = shared_from_this();
    try {
      auto ep = socket_.remote_endpoint();
      remote_host_ = ep.address().to_string();
      remote_port_ = ep.port();
    } catch (...) { remote_host_ = "?"; remote_port_ = 0; }
    std::cerr << "[p2p] peer " << id_ << " connected (" << label() << ")\n";
    do_read_header(self);
  }

  // ── Public helpers called from outside ───────────────────────────────

  /// Send a text SYNC notification (called from broadcast_sync_peers).
  void notify_sync() {
    if (!alive_) return;
    try {
      send_text("SYNC");
    } catch (...) {
      alive_ = false;
    }
  }

  /// Initiate a full sync cycle after connecting.
  void initiate_sync() {
    request_pull_async("initial");
  }

private:
  // ── State ────────────────────────────────────────────────────────────
  std::mutex sync_mutex_;
  bool pull_inflight_ = false;
  bool pull_pending_  = false;
  bool serving_pull_  = false;
  bool awaiting_done_ = false;
  bool done_received_ = false;
  std::deque<std::vector<char>> pending_binary_frames_;
  size_t pending_binary_bytes_ = 0;
  static constexpr size_t MAX_PENDING_BINARY_FRAMES = 1024;
  static constexpr size_t MAX_PENDING_BINARY_BYTES = 8 * 1024 * 1024;

  std::vector<char> read_buf_;
  uint32_t read_type_ = 0;
  uint32_t read_len_  = 0;

  // Active FSM wrappers — accessed from both the async read loop (dispatch)
  // and the sync threads.  Protected by g_db_mutex during sync.
  std::unique_ptr<ReplicationSender<P2pStorage>>   active_sender_;
  std::unique_ptr<ReplicationReceiver<P2pStorage>> active_receiver_;
  std::unique_ptr<TcpTransport>                     active_transport_;

  void enqueue_pending_binary_frame(const std::vector<char>& data) {
    if (pending_binary_frames_.size() >= MAX_PENDING_BINARY_FRAMES) {
      std::cerr << "[p2p] peer " << id_ << " dropping queued frame (count limit)\n";
      return;
    }
    if (pending_binary_bytes_ + data.size() > MAX_PENDING_BINARY_BYTES) {
      std::cerr << "[p2p] peer " << id_ << " dropping queued frame (byte limit)\n";
      return;
    }
    pending_binary_bytes_ += data.size();
    pending_binary_frames_.push_back(data);
  }

  void drain_pending_binary_frames_locked() {
    while (!pending_binary_frames_.empty() && alive_) {
      auto data = std::move(pending_binary_frames_.front());
      pending_binary_bytes_ -= data.size();
      pending_binary_frames_.pop_front();
      feed_fsm(data);
    }
  }

  bool wait_for_receiver_idle_or_error(SyncEvents& events,
                                       const char* phase) {
    auto start = std::chrono::steady_clock::now();
    while (alive_) {
      ReplicationState state = ReplicationState::IDLE;
      auto now = std::chrono::steady_clock::now();
      auto inactive_for = std::chrono::steady_clock::duration::zero();
      {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        if (!active_receiver_) return !events.errored;
        state = active_receiver_->state();
        inactive_for = now - active_receiver_->last_activity();
      }
      if (state != ReplicationState::ACTIVE) break;
      if ((now - start) > SYNC_IDLE_TIMEOUT || inactive_for > SYNC_IDLE_TIMEOUT) {
        events.errored = true;
        events.msg = "receiver timeout";
        std::cerr << "[p2p] peer " << id_ << " sync timeout in " << phase << "\n";
        alive_ = false;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !events.errored && alive_;
  }

  bool wait_for_done_or_error(const char* phase) {
    auto start = std::chrono::steady_clock::now();
    while (alive_) {
      bool done = false;
      {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        done = done_received_;
      }
      if (done) {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        done_received_ = false;
        awaiting_done_ = false;
        return true;
      }
      auto now = std::chrono::steady_clock::now();
      if ((now - start) > SYNC_IDLE_TIMEOUT) {
        std::cerr << "[p2p] peer " << id_ << " sync timeout in " << phase << "\n";
        alive_ = false;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  }

  bool wait_for_sender_idle_or_error(SyncEvents& events,
                                     const char* phase) {
    auto start = std::chrono::steady_clock::now();
    while (alive_) {
      ReplicationState state = ReplicationState::IDLE;
      auto now = std::chrono::steady_clock::now();
      auto inactive_for = std::chrono::steady_clock::duration::zero();
      {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        if (!active_sender_) return !events.errored;
        state = active_sender_->state();
        inactive_for = now - active_sender_->last_activity();
      }
      if (state != ReplicationState::ACTIVE) break;
      if ((now - start) > SYNC_IDLE_TIMEOUT || inactive_for > SYNC_IDLE_TIMEOUT) {
        events.errored = true;
        events.msg = "sender timeout";
        std::cerr << "[p2p] peer " << id_ << " sync timeout in " << phase << "\n";
        alive_ = false;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !events.errored && alive_;
  }

  // ── Async read chain ─────────────────────────────────────────────────

  void do_read_header(std::shared_ptr<PeerSession> self) {
    read_buf_.resize(FRAME_HDR);
    boost::asio::async_read(
        socket_, boost::asio::buffer(read_buf_),
        [this, self](boost::system::error_code ec, size_t) {
          if (ec || !alive_) { on_disconnect(ec); return; }
          memcpy(&read_type_, read_buf_.data(), 4);
          read_type_ = ntohl(read_type_);
          memcpy(&read_len_, read_buf_.data() + 4, 4);
          read_len_ = ntohl(read_len_);
          if (read_len_ > 64 * 1024 * 1024) {
            std::cerr << "[p2p] peer " << id_
                      << " frame too large\n";
            alive_ = false; return;
          }
          do_read_payload(self);
        });
  }

  void do_read_payload(std::shared_ptr<PeerSession> self) {
    if (read_len_ == 0) {
      dispatch_frame(Frame{read_type_, 0, {}});
      do_read_header(self);
      return;
    }
    read_buf_.resize(read_len_);
    boost::asio::async_read(
        socket_, boost::asio::buffer(read_buf_),
        [this, self](boost::system::error_code ec, size_t) {
          if (ec || !alive_) { on_disconnect(ec); return; }
          Frame f;
          f.type = read_type_;
          f.len  = read_len_;
          f.payload.assign(read_buf_.begin(), read_buf_.end());
          dispatch_frame(std::move(f));
          do_read_header(self);
        });
  }

  void on_disconnect(boost::system::error_code ec) {
    if (ec && ec != boost::asio::error::eof
           && ec != boost::asio::error::connection_reset) {
      std::cerr << "[p2p] peer " << id_ << " error: " << ec.message() << "\n";
    }
    alive_ = false;
    remove_from_peers();
  }

  void remove_from_peers() {
    std::lock_guard<std::mutex> lock(g_peers_mutex);
    g_peers.erase(std::remove_if(g_peers.begin(), g_peers.end(),
                      [this](const auto& p) { return p->id_ == id_; }),
                  g_peers.end());
  }

  // ── Frame dispatch (runs on io_context thread) ──────────────────────

  void dispatch_frame(Frame f) {
    if (f.type == FRAME_BINARY) {
      std::lock_guard<std::mutex> lock(sync_mutex_);
      if (!active_receiver_ && !active_sender_) {
        enqueue_pending_binary_frame(f.payload);
        return;
      }
      feed_fsm(f.payload);
      return;
    }
    // Text frame
    std::string msg(f.payload.begin(), f.payload.end());
    std::cerr << "[p2p] peer " << id_ << " text: " << msg << "\n";

    if (msg == "SYNC") {
      request_pull_async("sync hint");
    } else if (msg == "PULL") {
      serve_pull_async();
    } else if (msg == "DONE") {
      std::lock_guard<std::mutex> lock(sync_mutex_);
      if (awaiting_done_) done_received_ = true;
    }
  }

  void feed_fsm(const std::vector<char>& data) {
    std::lock_guard<std::mutex> db_lock(g_db_mutex);
    if (active_receiver_) {
      PeerCommitScope scope(id_);
      auto& rb = active_receiver_->receive_buffer();
      auto* src = reinterpret_cast<const uint8_t*>(data.data());
      size_t todo = data.size();
      size_t off = 0;
      while (off < todo) {
        size_t chunk = std::min(todo - off, rb.available());
        if (chunk == 0) break;
        memcpy(rb.write_ptr(), src + off, chunk);
        rb.advance(chunk);
        off += chunk;
        active_receiver_->on_data_received();
      }
    }
    if (active_sender_) {
      active_sender_->on_message_received(
          reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }
  }

  // ── Command-driven sync operations (runs on background threads) ──────

  void request_pull_async(const char* reason) {
    bool should_start = false;
    {
      std::lock_guard<std::mutex> lock(sync_mutex_);
      if (!alive_) return;
      if (pull_inflight_) {
        pull_pending_ = true;
        std::cerr << "[p2p] peer " << id_ << " coalescing pull (" << reason
                  << ")\n";
        return;
      }
      pull_inflight_ = true;
      pull_pending_ = false;
      awaiting_done_ = false;
      done_received_ = false;
      should_start = true;
    }
    if (!should_start) return;
    std::thread([self = shared_from_this(), reason_str = std::string(reason)] {
      self->run_pull_from_peer(reason_str.c_str());
    }).detach();
  }

  void serve_pull_async() {
    bool should_start = false;
    {
      std::lock_guard<std::mutex> lock(sync_mutex_);
      if (!alive_) return;
      if (serving_pull_) return;
      serving_pull_ = true;
      should_start = true;
    }
    if (!should_start) return;
    std::thread([self = shared_from_this()] { self->run_serve_pull_to_peer(); })
        .detach();
  }

  void finish_pull_cycle(bool success) {
    bool rerun = false;
    {
      std::lock_guard<std::mutex> lock(sync_mutex_);
      rerun = pull_pending_;
      pull_pending_ = false;
      pull_inflight_ = false;
      awaiting_done_ = false;
      done_received_ = false;
      active_receiver_.reset();
      if (!active_sender_) active_transport_.reset();
      if (!success) {
        pending_binary_frames_.clear();
        pending_binary_bytes_ = 0;
      }
    }
    if (rerun && alive_) request_pull_async("coalesced");
  }

  void run_pull_from_peer(const char* reason) {
    auto storage = g_storage;
    if (!storage) {
      finish_pull_cycle(false);
      return;
    }
    auto db = storage->template open<P2pStorage::ReplicationDB>("main");
    SyncEvents events;
    std::cerr << "[p2p] peer " << id_ << " start pull (" << reason << ")\n";

    {
      {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        awaiting_done_ = false;
        done_received_ = false;
        active_receiver_ = std::make_unique<ReplicationReceiver<P2pStorage>>(db);
        active_transport_ = std::make_unique<TcpTransport>(socket_);
        {
          std::lock_guard<std::mutex> db_lock(g_db_mutex);
          active_receiver_->begin(active_transport_.get(), &events);
        }
        drain_pending_binary_frames_locked();
      }

      try {
        send_text("PULL");
      } catch (...) {
        alive_ = false;
      }

      if (!alive_) {
        finish_pull_cycle(false);
        return;
      }
      {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        awaiting_done_ = true;
      }

      if (!wait_for_receiver_idle_or_error(events, "pull/receive")) {
        finish_pull_cycle(false);
        return;
      }

      if (!wait_for_done_or_error("pull/done")) {
        finish_pull_cycle(false);
        return;
      }
    }

    finish_pull_cycle(true);
    std::cerr << "[p2p] sync with peer " << id_ << " complete\n";
  }

  void run_serve_pull_to_peer() {
    auto storage = g_storage;
    if (!storage) {
      std::lock_guard<std::mutex> lock(sync_mutex_);
      serving_pull_ = false;
      return;
    }
    auto db = storage->template open<P2pStorage::ReplicationDB>("main");
    SyncEvents events;
    {
      {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        active_sender_ = std::make_unique<ReplicationSender<P2pStorage>>(db);
        active_transport_ = std::make_unique<TcpTransport>(socket_);
        {
          std::lock_guard<std::mutex> db_lock(g_db_mutex);
          active_sender_->begin(active_transport_.get(), &events);
        }
        drain_pending_binary_frames_locked();
      }

      if (!wait_for_sender_idle_or_error(events, "serve-pull/send")) {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        active_sender_.reset();
        if (!active_receiver_) active_transport_.reset();
        serving_pull_ = false;
        return;
      }
      {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        active_sender_.reset();
        if (!active_receiver_) active_transport_.reset();
      }
    }

    try {
      send_text("DONE");
    } catch (...) {
      alive_ = false;
    }

    {
      std::lock_guard<std::mutex> lock(sync_mutex_);
      serving_pull_ = false;
    }
  }

  void send_text(const std::string& msg) {
    Frame f;
    f.type = FRAME_TEXT;
    f.len  = static_cast<uint32_t>(msg.size());
    f.payload.assign(msg.begin(), msg.end());
    auto wire = f.encode();
    boost::system::error_code ec;
    boost::asio::write(socket_, boost::asio::buffer(wire), ec);
  }
};

static void connect_to_peer_async(const std::string& host, uint16_t port) {
  if (!g_io) return;

  net::post(*g_io, [host, port] {
    {
      std::lock_guard<std::mutex> lock(g_peers_mutex);
      for (auto& p : g_peers) {
        if (p && p->alive_.load() && p->remote_host_ == host &&
            p->remote_port_ == port)
          return;
      }
    }

    auto resolver = std::make_shared<tcp::resolver>(*g_io);
    resolver->async_resolve(
        host, std::to_string(port),
        [resolver, host, port](boost::system::error_code ec,
                               tcp::resolver::results_type endpoints) {
          if (ec) {
            std::cerr << "[p2p] manual connect resolve failed " << host << ":"
                      << port << " : " << ec.message() << "\n";
            return;
          }

          auto session = std::make_shared<PeerSession>(
              *g_io, g_next_manual_peer_id.fetch_add(1));
          boost::asio::async_connect(
              session->socket_, endpoints,
              [session, host, port](boost::system::error_code ec,
                                    tcp::endpoint) {
                if (ec) {
                  std::cerr << "[p2p] manual connect failed " << host << ":"
                            << port << " : " << ec.message() << "\n";
                  return;
                }
                {
                  std::lock_guard<std::mutex> lock(g_peers_mutex);
                  g_peers.push_back(session);
                }
                std::cerr << "[p2p] manually connected to " << host << ":"
                          << port << "\n";
                session->start_read_loop();
                session->initiate_sync();
              });
        });
  });
}

// ── Broadcast SYNC to all peers (excluding specified IDs) ──────────────────

static void broadcast_sync_peers(const std::set<int>& excluded) {
  std::vector<std::shared_ptr<PeerSession>> peers;
  {
    std::lock_guard<std::mutex> lock(g_peers_mutex);
    peers = g_peers;
  }
  for (auto& p : peers) {
    if (!p || !p->alive_.load()) continue;
    if (excluded.count(p->id_)) continue;
    p->notify_sync();
  }
}

// ── Discovery ──────────────────────────────────────────────────────────────

class Discovery {
public:
  Discovery(net::io_context& io, uint16_t tcp_port)
      : io_(io), socket_(io), timer_(io), tcp_port_(tcp_port),
        instance_id_(uint32_t(std::random_device{}())) {
    socket_.open(udp::v4());
    socket_.set_option(boost::asio::ip::multicast::enable_loopback(true));
    socket_.set_option(boost::asio::ip::multicast::hops(1));
    socket_.set_option(udp::socket::reuse_address(true));
    socket_.bind(udp::endpoint(udp::v4(), MULTICAST_PORT));
    boost::asio::ip::multicast::join_group join(MULTICAST_ADDR);
    socket_.set_option(join);
    socket_.set_option(udp::socket::broadcast(true));

    start_receive();
    announce();
    start_timer();
  }

private:
  net::io_context& io_;
  udp::socket      socket_;
  net::steady_timer timer_;
  uint16_t         tcp_port_;
  uint32_t         instance_id_;
  std::array<char, 256> recv_buf_;
  udp::endpoint    remote_ep_;
  int next_id_ = 100;

  std::string instance_str() const {
    char buf[16];
    snprintf(buf, sizeof(buf), "%08x", instance_id_);
    return buf;
  }

  void start_receive() {
    socket_.async_receive_from(
        net::buffer(recv_buf_), remote_ep_,
        [this](boost::system::error_code ec, size_t len) {
          if (ec) { start_receive(); return; }
          handle_received(len);
          start_receive();
        });
  }

  void start_timer() {
    timer_.expires_after(ANNOUNCE_INTERVAL);
    timer_.async_wait([this](boost::system::error_code ec) {
      if (ec) return;
      announce();
      start_timer();
    });
  }

  void announce() {
    std::string msg = instance_str() + ":" + std::to_string(tcp_port_);
    socket_.async_send_to(
        net::buffer(msg), udp::endpoint(MULTICAST_ADDR, MULTICAST_PORT),
        [](boost::system::error_code, size_t) {});
  }

  void handle_received(size_t len) {
    std::string msg(recv_buf_.data(), len);
    auto colon = msg.find(':');
    if (colon == std::string::npos) return;
    std::string inst = msg.substr(0, colon);
    if (inst == instance_str()) return;

    int port = 0;
    try { port = std::stoi(msg.substr(colon + 1)); } catch (...) { return; }
    if (port <= 0 || port > 65535) return;

    auto remote_addr = remote_ep_.address();
    if (!remote_addr.is_v4()) return;
    auto addr = remote_addr.to_string();

    // Check if already connected
    {
      std::lock_guard<std::mutex> lock(g_peers_mutex);
      for (auto& p : g_peers) {
        if (p && p->remote_host_ == addr && p->remote_port_ == port)
          return;
      }
    }

    std::cerr << "[p2p] discovered peer " << addr << ":" << port << "\n";
    connect_to(addr, static_cast<uint16_t>(port));
  }

  void connect_to(const std::string& host, uint16_t port) {
    auto resolver = std::make_shared<tcp::resolver>(io_);
    resolver->async_resolve(host, std::to_string(port),
        [this, resolver](boost::system::error_code ec,
                         tcp::resolver::results_type endpoints) {
          if (ec) return;
          do_connect(endpoints);
        });
  }

  void do_connect(tcp::resolver::results_type endpoints) {
    auto session = std::make_shared<PeerSession>(io_, ++next_id_);
    boost::asio::async_connect(
        session->socket_, endpoints,
        [this, session](boost::system::error_code ec, tcp::endpoint) {
          if (ec) return;
          {
            std::lock_guard<std::mutex> lock(g_peers_mutex);
            g_peers.push_back(session);
          }
          session->start_read_loop();
          session->initiate_sync();
        });
  }
};

// ── Acceptor ───────────────────────────────────────────────────────────────

class Acceptor {
public:
  Acceptor(net::io_context& io, uint16_t port)
      : io_(io), acceptor_(io, tcp::endpoint(tcp::v4(), port)) {
    do_accept();
  }

private:
  net::io_context& io_;
  tcp::acceptor acceptor_;
  int next_id_ = 1;

  void do_accept() {
    auto session = std::make_shared<PeerSession>(io_, next_id_++);
    acceptor_.async_accept(
        session->socket_,
        [this, session](boost::system::error_code ec) {
          if (!ec) {
            {
              std::lock_guard<std::mutex> lock(g_peers_mutex);
              g_peers.push_back(session);
            }
            session->start_read_loop();
          }
          do_accept();
        });
  }
};

// ── Terminal UI ────────────────────────────────────────────────────────────

static void print_help() {
  std::cout <<
    "Commands:\n"
    "  add <key> <value>   Insert or update (stores UTC timestamp)\n"
    "  get <key>           Read payload and UTC timestamp\n"
    "  del <key>           Delete a key\n"
    "  list                List keys with payload and UTC timestamp\n"
    "  peers               Show connected peers\n"
    "  connect <h> <p>     Manually connect to peer host/port\n"
    "  help                Show this help\n"
    "  quit                Exit\n";
}

static bool handle_command(const std::string& line) {
  std::istringstream iss(line);
  std::string cmd;
  iss >> cmd;

  if (cmd == "quit" || cmd == "exit") return false;
  if (cmd == "help") { print_help(); return true; }

  if (cmd == "add" || cmd == "put") {
    std::string key, value;
    iss >> key;
    std::getline(iss, value);
    if (key.empty()) { std::cout << "Usage: add <key> <value>\n"; return true; }
    if (!value.empty() && value[0] == ' ') value.erase(0, 1);

    std::lock_guard<std::mutex> lock(g_db_mutex);
    auto db = g_storage->template open<P2pStorage::ReplicationDB>("main");
    auto c = db.cursor();
    uint64_t ts = utc_epoch_ms_now();
    std::string encoded = encode_timestamped_value(ts, value);
    c.start_transaction();
    c.find(Slice(key));
    c.value(Slice(encoded));
    c.commit();
    std::cout << "added: " << key << " = " << value
              << " (utc_ms=" << ts << ")\n";
    return true;
  }

  if (cmd == "get") {
    std::string key;
    iss >> key;
    if (key.empty()) { std::cout << "Usage: get <key>\n"; return true; }
    std::lock_guard<std::mutex> lock(g_db_mutex);
    auto db = g_storage->template open<P2pStorage::ReplicationDB>("main");
    auto c = db.cursor();
    c.find(Slice(key));
    if (c.is_valid())
      std::cout << key << " = " << format_value_for_display(c.value())
                << "\n";
    else
      std::cout << key << " not found\n";
    return true;
  }

  if (cmd == "del" || cmd == "delete" || cmd == "remove") {
    std::string key;
    iss >> key;
    if (key.empty()) { std::cout << "Usage: del <key>\n"; return true; }
    std::lock_guard<std::mutex> lock(g_db_mutex);
    auto db = g_storage->template open<P2pStorage::ReplicationDB>("main");
    auto c = db.cursor();
    c.start_transaction();
    c.find(Slice(key));
    if (c.is_valid()) {
      c.remove();
      std::cout << "deleted: " << key << "\n";
    } else {
      std::cout << key << " not found\n";
    }
    c.commit();
    return true;
  }

  if (cmd == "list") {
    std::lock_guard<std::mutex> lock(g_db_mutex);
    auto db = g_storage->template open<P2pStorage::ReplicationDB>("main");
    auto c = db.cursor();
    c.first();
    if (!c.is_valid()) {
      std::cout << "(empty)\n";
      return true;
    }
    int n = 0;
    do {
      std::cout << c.key().string() << " = "
                << format_value_for_display(c.value()) << "\n";
      ++n;
      c.next();
    } while (c.is_valid());
    std::cout << "--- " << n << " item(s) ---\n";
    return true;
  }

  if (cmd == "peers") {
    std::lock_guard<std::mutex> lock(g_peers_mutex);
    if (g_peers.empty()) {
      std::cout << "No connected peers.\n";
      return true;
    }
    std::cout << "Connected peers:\n";
    for (auto& p : g_peers)
      std::cout << "  " << p->id_ << ": " << p->label() << "\n";
    return true;
  }

  if (cmd == "connect") {
    std::string host;
    int port = 0;
    iss >> host >> port;
    if (host.empty() || port <= 0 || port > 65535) {
      std::cout << "Usage: connect <host> <port>\n";
      return true;
    }
    connect_to_peer_async(host, static_cast<uint16_t>(port));
    std::cout << "connecting to " << host << ":" << port << "...\n";
    return true;
  }

  std::cout << "Unknown command: " << cmd << " (type 'help')\n";
  return true;
}

// ── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
  uint16_t port = 0;
  std::string db_path;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--port" && i + 1 < argc)
      port = static_cast<uint16_t>(std::atoi(argv[++i]));
    else if (arg == "--db" && i + 1 < argc)
      db_path = argv[++i];
  }

  if (port == 0 || db_path.empty()) {
    std::cerr << "Usage: " << argv[0] << " --port <tcp_port> --db <db_path>\n";
    return 1;
  }

  g_storage = P2pStorage::create(db_path.c_str());
  { auto db = g_storage->template open<P2pStorage::ReplicationDB>("main"); (void)db; }
  std::cerr << "[p2p] storage: " << db_path << "\n";

  net::io_context io(1);
  g_io = &io;

  P2pSyncNotifier notifier(io);
  g_notifier = &notifier;

  Discovery discovery(io, port);
  Acceptor acceptor(io, port);
  std::cerr << "[p2p] listening on port " << port << "\n";

  std::thread io_thread([&] {
    try { io.run(); }
    catch (std::exception& e) { std::cerr << "[p2p] io error: " << e.what() << "\n"; }
  });

  std::cout << "READY " << port << std::endl;
  print_help();

  std::string line;
  while (std::cout << "> " && std::getline(std::cin, line)) {
    if (!handle_command(line)) break;
  }

  std::cerr << "[p2p] shutting down...\n";
  io.stop();
  io_thread.join();

  {
    std::lock_guard<std::mutex> lock(g_peers_mutex);
    for (auto& p : g_peers) p->close();
    g_peers.clear();
  }

  g_notifier = nullptr;
  g_io = nullptr;
  g_storage.reset();
  std::cerr << "[p2p] done.\n";
  return 0;
}