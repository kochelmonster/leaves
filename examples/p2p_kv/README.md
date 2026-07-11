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

If multicast discovery is blocked in your environment (VPN/container/firewall),
manually bootstrap a connection from one peer terminal:

```text
connect 127.0.0.1 9002
```

Once one TCP session is established, replication works normally.

## Commands

| Command               | Description              |
|-----------------------|--------------------------|
| `add <key> <value>`   | Insert or update a key   |
| `get <key>`           | Read a key's value       |
| `del <key>`           | Delete a key             |
| `list`                | List all keys            |
| `peers`               | Show connected peers     |
| `connect <h> <p>`     | Manual peer bootstrap via TCP |
| `help`                | Show help                |
| `quit`                | Exit                     |

`add` stores values in this demo format:

`<utc_epoch_ms>|<payload>`

The terminal output for `get` and `list` decodes this format and shows both
payload and timestamp.

## Conflict Resolution Demo

This example demonstrates conflict resolution via the Aspect merge pointcut
`may_merge_overwrite`.

- The value contains a UTC timestamp (Unix epoch milliseconds).
- During replication merge, `may_merge_overwrite` compares destination and
	source timestamps.
- The value with the younger timestamp (larger UTC epoch ms) wins.
- If timestamps are equal, the existing destination value is kept.

### Quick two-peer walkthrough

1. Start two peers on different ports and DB files.
2. On peer A: `add k old_from_a`
3. Very shortly after, on peer B: `add k new_from_b`
4. Let sync complete.
5. On both peers: `get k`

Expected: both peers converge to the payload with the newer UTC timestamp.

## Protocol

The control plane is command-driven (similar to the browser demo):

- `SYNC` is only a notification hint.
- Receiver of `SYNC` starts a pull from that peer.
- `PULL` means "send me your current trie snapshot".
- Sender streams binary replication frames and then sends `DONE`.

Initial connection triggers an outbound `PULL` so the new peer converges
to current state quickly.

Typical flow after a local commit:

```
Peer A (committer) ── "SYNC" ───────────────────────→ Peer B
Peer B ─────────────── "PULL" ───────────────────────→ Peer A
Peer A ── binary trie frames (ReplicationSender) ───→ Peer B
Peer A ── "DONE" ───────────────────────────────────→ Peer B
```

Commits still use a 100ms debounce and origin-peer exclusion, so a peer
does not immediately echo notifications back to the source peer.

## Requirements

- CMake 3.9+
- Ninja (or Make)
- Boost.Asio 1.80+
- C++20 compiler
- Linux with IPv4 multicast support