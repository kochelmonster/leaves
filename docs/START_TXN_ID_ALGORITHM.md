# `_start_txn_id`: Per-Context GC Threshold

## Background

Each write context (`_TxnContext`) has its own garbage queue embedded in
`mem_manager`.  When a page is freed during a write transaction with id `T`,
`mark_for_recycle` stamps the queue entry:

```cpp
garbage_block.txn_id = _active_txn->txn_id;   // = T
```

Later, when a new transaction recycles that entry, `may_recycle` checks:

```cpp
if (!(garbage_block.txn_id < _ctx->_start_txn_id))
    return RecycleResult::STOP;
```

A page is only recycled when `T < _start_txn_id`.  The queue is ordered by
`txn_id` within a context, so `STOP` terminates the scan early.

`_start_txn_id` is therefore a **safe-recycle threshold**: all pages whose
queue entry has `txn_id < _start_txn_id` are guaranteed to be unreachable by
any live reader.

---

## The Algorithm

`start_transaction` sets `_start_txn_id` in two steps, both performed while
holding `txn_ref_lock`.

### Step 1 — Pessimistic initialisation

```cpp
ctx->_start_txn_id = active->txn_id = ++_header->_last_assigned_tid;
```

`active->txn_id` is the ID assigned to the transaction about to start.  No
committed txn in the chain has an ID this high yet, so this value allows
**zero** recycling.  It is a safe default.

### Step 2 — Refinement via GC chain walk

`iter_transactions` walks the chain from oldest to newest:

1. **Dead transactions** (refs == 0) before the first live one are freed
   immediately.
2. **First live transaction** is found (refs > 0).  `found_live` is set to
   true; chain head is advanced to this transaction.
3. **Continue scanning** (still oldest→newest) past all live transactions.
   For each transaction committed by **this context** (`_context_id ==
   my_ctx_id`), update:

```cpp
ctx->_start_txn_id = txn->txn_id;
return true;   // stop — this is the most recently seen own-context txn
```

After the walk, `_start_txn_id` equals the txn_id of the **oldest live
transaction from this context** that remains in the chain.  All queue entries
with `txn_id` below that value were freed before any currently-live reader
existed and are therefore safe to recycle.

---

## Proof of Correctness

### Safety — no live reader can reach a recycled page

Let `T_safe = _start_txn_id` after the walk.  By construction, `T_safe` is
the txn_id of some transaction `txn_C` committed by context `C`, where
`txn_C` is at or after the oldest live transaction in the chain.

A garbage entry with `txn_id = T < T_safe` was stamped when context `C` freed
a page during transaction `T`.  Because all transactions with id ≤ `T` are
older than `txn_C`, and `txn_C` is at or after the oldest live reader, no live
reader started before `T + 1`.  Therefore no live reader holds a snapshot
that could reference the page.  The recycle is safe.

### Monotonicity — `_start_txn_id` only advances within a context

`_last_assigned_tid` is a global monotonic counter incremented under
`txn_ref_lock`.  A context's committed transactions appear in the chain in
strictly ascending txn_id order (each was assigned a higher id than the last).

On successive calls to `start_transaction` for the same context, the chain
walk finds the oldest live own-context transaction.  Because the chain only
grows forward and dead txns are pruned from the front, the oldest live
own-context txn_id can only stay the same or increase.

Therefore `_start_txn_id` is **non-decreasing** across successive
transactions of the same context, which is required by the `STOP` semantics in
`may_recycle`.

### Why the value must come from this context's own transactions

Suppose `_start_txn_id` were set to the txn_id of a transaction from **another
context** `D`.

Context `D` and context `C` interleave arbitrarily.  On the next call to
`start_transaction` for context `C`, the oldest live txn from `D` could have a
lower id than last time (a new, older txn from `D` appearing earlier in the
chain after a merge), or the txn from `D` that was used last time might have
been freed.  In either case `_start_txn_id` could **decrease**, breaking the
`STOP` invariant.

By contrast, context `C`'s own committed txn_ids advance monotonically (each
call to `commit` for `C` appends a strictly-higher txn_id to the chain), so
the value sourced from `C`'s own chain is guaranteed non-decreasing.

---

## Summary

| Value | Meaning | Safety |
|---|---|---|
| `active->txn_id` (step 1) | Highest possible; no recycling | Always safe |
| Own-context chain txn_id (step 2) | Maximum refinement | Safe because all lower-id readers are gone; monotonic because own-context ids only advance |
| Any other context's txn_id | — | **Unsafe**: not monotonic across transactions |
