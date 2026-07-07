# P2P Key-Value Store

Peer-to-peer key-value store with no central server. Peers discover
each other over UDP multicast, connect via TCP, and continuously
synchronise their databases using the LVRP replication protocol.

Every instance is a full peer — there is no server role.

## Architecture

```
┌─ Terminal UI (main thread) ──────────────────────┐
│  Commands: add, get, del, list, peers, quit       │
│  Posts commits to io_context via thread-safe       │
│  access under g_db_mutex                           │
└───────────────────────────────────────────────────┘

┌─ io_context (background thread) ──────────────────┐
│                                                    │
│  UDP Discovery:                                    │
│   • Multicast listen on 239.255.0.1:9876           │
│   • Periodic announce (every 30s)                  │
│   • On new peer → async_connect + PeerSession      │
│                                                    │
│  TCP Acceptor:                                     │
│   • async_accept on --port                         │
│   • On connect → start PeerSession read loop       │
│                                                    │
│  PeerSession (one per peer):                       │
│   • Length-framed: [uint32_t type][uint32_t len]   │
│   • type=0 → binary LVRP payload (FSM data)        │
│   • type=1 → text control (SYNC/PULL/DONE)         │
│   • Async read loop dispatches to active FSM       │
│                                                    │
│  Sync Notifier (debounced):                        │
│   • Aspect::on_commit → 100ms timer                │
│   → sends "SYNC" to all peers (excluding origin)   │
└────────────────────────────────────────────────────┘
```

## Build

```bash
# From examples/p2p_kv:
cmake -B build -G Ninja
cmake --build build -j --target p2p_kv
```

### Debug Build

To build with debug symbols and optimisation disabled:

```bash
# Configure from scratch:
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j --target p2p_kv
```

Debug mode enables verbose diagnostic output (`[dbg]` lines) in the LVRP
replication FSMs, showing session IDs, node counts, big-value transfers, and
state transitions.

## Usage

Start two peers on separate terminals (different ports, different DB files):

```bash
# Terminal 1:
./p2p_kv --port 9001 --db ./peer1.lvs

# Terminal 2:
./p2p_kv --port 9002 --db ./peer2.lvs
```

The peers discover each other automatically via UDP multicast and
establish a TCP connection. An initial bidirectional sync transfers
all data. After that, any change on one peer is automatically
propagated to the other.

## Commands

| Command               | Description              |
|-----------------------|--------------------------|
| `add <key> <value>`   | Insert or update a key   |
| `get <key>`           | Read a key's value       |
| `del <key>`           | Delete a key             |
| `list`                | List all keys            |
| `peers`               | Show connected peers     |
| `help`                | Show help                |
| `quit`                | Exit                     |

## Protocol

After connection, an initial bidirectional sync runs:

```
Peer A ── "SYNC" ────────────────────────────────────→ Peer B
Peer A ←─ Phase 1: B sends trie, A receives ────────── Peer B
Peer A ── "PULL" ────────────────────────────────────→ Peer B
Peer A ── Phase 2: A sends trie, B receives ─────────→ Peer B
Peer A ── "DONE" ────────────────────────────────────→ Peer B
```

After any local commit, the committer debounces 100ms then sends
a text "SYNC" notification to all connected peers (except the one
that originated the change), which triggers a fresh sync cycle.

## Requirements

- CMake 3.9+
- Ninja (or Make)
- Boost.Asio 1.80+
- C++20 compiler
- Linux with IPv4 multicast support