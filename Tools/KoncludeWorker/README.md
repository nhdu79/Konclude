# Konclude worker process (prototype)

Runs Konclude in a child process that the host restarts every N state
changes. Process exit frees all of the worker's memory, including what
Konclude still leaks per state change (much less since task 5d; see
`docs/INCREMENTAL_WORK_LOG.md`, Tasks 5c and 5d), so memory stays bounded by
what one worker accumulates in N changes.

## How it works

1. **Start.** `KoncludeWorkerClient` creates a `socketpair`, then `fork` +
   `exec`s `konclude_worker 3` with its end of the socket on fd 3. `exec` gives
   the worker a clean process: forking a process that already runs Konclude
   would leave the child without Konclude's threads (only the forking thread
   survives a fork) and with their locks possibly held.
2. **Requests.** Each client call sends one frame and reads one response
   (`konclude_worker_protocol.h`: a kind plus length-prefixed strings; `send`
   with `MSG_NOSIGNAL`, so a dead worker gives an error instead of `SIGPIPE`).
   The worker maps them 1:1 onto the embedded C API
   (`Source/Control/Interface/Embedded/konclude_embedded.h`) of its one handle.
3. **State tracking.** After every successful tell/retract, the client updates
   its copy of the current state (sets of class and object-property facts).
4. **Restart.** Before a state change, once `restartEvery` changes ran on the
   current worker, the client kills it (`SIGKILL`: no teardown needed), starts
   a new one, loads the ontology, and tells the whole current state in one
   change. A worker that died (crash, kill) is restarted the same way and the
   failed request is retried once.

The client needs only `konclude_embedded.h` (for the assertion structs); it
does not link libKonclude or Qt. Its methods mirror the embedded C API:
`ok`, `loadOntology`, `tellAndRetract`, `tellAndRetractAssertions`, `query`
and result accessors, consistency/satisfiability checks, `lastError`, and
diagnostics (`workerPid`, `workerRssKb`, `restartCount`).

Powerlifted compiles this client into its search (`build.py --konclude`)
and uses it through `src/search/konclude/state_reasoner.h` there; see
`docs/EMBEDDED_INTERFACE.md` §5.

## Build and test (from the repo root)

```sh
Tools/EmbeddedDriver/rebuild.sh     # ReleaseEmbedded/libKonclude.so
Tools/KoncludeWorker/build.sh       # the three binaries below, at the repo root
./konclude_worker_equivalence_test  # exit code 0 = same answers in-process and via the worker
./konclude_worker_growth_benchmark 1000 50
```

- `konclude_worker_equivalence_test [steps] [restartEvery]`: a seeded random
  sequence of class and property tells/retracts on
  `Tests/embedded-state-tbox-roles.owl.xml`; after every change it compares
  three queries in-process vs. via the worker, which restarts every 7 changes
  and is killed twice. 150 steps, 22 restarts: all answers equal.
- `konclude_worker_growth_benchmark [states] [restartEvery] [reportEvery]`:
  the workload of `embedded_incremental_growth_benchmark` (state grows by 1–2
  facts per change, 2 queries per state) through the worker.

## Results (1000 changes, state grows to 1,500 facts)

Measured after the leak fixes of task 5d (`docs/INCREMENTAL_WORK_LOG.md`);
the numbers before them are in parentheses.

| restartEvery | max worker RSS | total time | restarts |
|---|---|---|---|
| 50 | 84 MB (175 MB) | 15.9 s (17.7 s) | 19 |
| 100 | 121 MB (302 MB) | 15.0 s (18.8 s) | 9 |
| 200 | 191 MB (538 MB) | 15.6 s (19.1 s) | 4 |
| never | 544 MB (1.54 GB) | 16.5 s (20.6 s) | 0 |

Restarting does not cost time here: a restart (process start, load, re-tell,
first query) takes tens of milliseconds, and a younger worker answers faster.
The time per state grows with the state size either way (each queried state
is rebuilt from the basement, task 1). Memory per worker grows with both the
number of changes and the state size, so pick `restartEvery` for the largest
states expected.

## Limits of the prototype

- One request at a time; no timeouts (a hung worker blocks the caller).
- The worker binary is found by path (`./konclude_worker` in the tests).
- Facts are tracked as sets: telling a fact twice and retracting it once
  removes it.
