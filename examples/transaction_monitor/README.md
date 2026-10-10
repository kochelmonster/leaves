# Transaction Monitor Example

This example coordinates a durable two-phase commit across the `accounts` and
`audit` databases in one memory-mapped Leaves storage file. The monitor stores
its decision and participant progress in the `_transaction_monitor` database.
The caller registers both participants, calls `start()`, then writes through
their cursors. It chooses `commit()` or `rollback()` to finish the transaction.
On recovery, the persisted decision takes precedence: a durable commit cannot
be changed to an abort, and an undecided `preparing` transaction is presumed
aborted.

## Build and run

From the repository root:

```bash
cmake -S examples/transaction_monitor -B build-transaction-monitor -G Ninja
cmake --build build-transaction-monitor --target transaction_monitor
./build-transaction-monitor/transaction_monitor
```

To explicitly roll back both participant transactions and verify that neither
write is visible:

```bash
./build-transaction-monitor/transaction_monitor --rollback
```

To demonstrate recovery after the commit decision has been persisted but
before participant finalization:

```bash
./build-transaction-monitor/transaction_monitor --crash-after-decision
# This intentionally exits with status 42.
./build-transaction-monitor/transaction_monitor --recover
```

The recovery run calls `commit()` to resume the persisted `committing` state.
`commit()` and `rollback()` return pending progress when participant
finalization needs to be retried. A durable `committing` decision is never
rolled back.

The presumed-abort path can also be demonstrated. This crash occurs after one
participant has prepared but before the commit decision exists:

```bash
./build-transaction-monitor/transaction_monitor --crash-during-prepare
# This intentionally exits with status 43.
./build-transaction-monitor/transaction_monitor --recover
```

The example requires a C++20 compiler, CMake 3.22 or newer, Boost 1.80 or
newer, and a native Threads implementation. It creates a temporary
`leaves_transaction_monitor.lvs` file while running and removes it before
exiting.