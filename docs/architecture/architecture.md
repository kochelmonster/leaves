# Leaves Architecture

This document describes the overall architecture of Leaves before diving into per-subsystem implementation details.

## Scope and audience
This document is for developers and contributors who need a system-level model of Leaves components, boundaries, and design intent.

## Design principle: mechanism and policy separation
Leaves enforces a strict split between mechanism and policy:
- Mechanisms are provided by Leaves: trie storage, transaction flow, replication FSMs, serialization, and merge plumbing.
- Policies are provided by applications: consistency semantics, conflict resolution, merge behavior, and transport choices.

This separation is central to the architecture and appears in all subsystem interfaces.

## General Overview
![Leaves architecture overview](architecture-overview.svg)

Major components:
- Storage layer: memory-mapped, browser persistence backends.
- Every Storage can hold multiple databases.
- Every Database can hold multiple cursors.
- Extensibility layer: Aspects and handler/policy hooks.

In C++ multiple Database type are provided:
- `MapStorage::DB`  a non replicable single writer database, that provides ACID and Two-Phase Commit semantics.
- `MapStorage::ReplicationDB` like `DB` but adds replication support.
- `MapStorage::ConfluenceDB` a multi-writer database that provides simultaneous multi writing.
- `MapStorage::ConfluenceReplicationDB` like `ConfluenceDB` but adds replication support.

## Public and Internal types

The public API is a facade over the internal types.  Public types are defined in `include/leaves/*.hpp` and internal types are defined in `include/leaves/intern/*/*.hpp`.  Internal types are not part of the public API and may change without notice.


## Internal Subsystems
![Leaves subsystems](subsystems.svg)

- DB contains trie-manipulation and transactional database machinery, including _DB, _Cursor, _Inserter, _Deleter, and aspect integration via DefaultAspect.
- Core contains low-level shared primitives used across subsystems, including _TrieNode and _LeafNode layouts, pointer/offset traits, and Slice/value utility types.
- Memory contains allocation and reclamation infrastructure, including _BigMemory, _MemManager, _MemManagerPool, _GarbageSlot, Area, and AreaPool for page and big-value lifecycle management.
- Util contains reusable cross-cutting helpers such as _ThreadPoolMixin, _Merger, and _TransferTrie that support parallel work, merge flows, and transfer serialization.
- Storage contains backend implementations and persistence adapters, including _MemoryMapFile, _CacheStore, and _BrowserStore, plus backend-specific helpers for DB directory and WAL integration.
- Multi contains Confluence multi-writer internals, including _ConfluenceDB, _ConfluenceCursor, and _TributaryDB for tributary coordination and merge orchestration.
- Replication contains replication-aware wrappers and protocol state machines, including _ReplicationDB, replication cursors, _HashUpdater, ReplicationSenderFSM, and ReplicationReceiverFSM.


