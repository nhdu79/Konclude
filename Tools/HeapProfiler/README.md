# Rough live-heap profiler (diagnosis only)

An `LD_PRELOAD` library that records the size and a 14-frame backtrace of
every live `malloc`, and writes the top 60 call stacks by live bytes at exit.
It was used for task 5 (`docs/INCREMENTAL_WORK_LOG.md`).

```sh
gcc -O1 -shared -fPIC -o heapprof.so Tools/HeapProfiler/heapprof.c -ldl -lpthread
timeout 600 env HEAPPROF_OUT=/tmp/live-15 LD_PRELOAD=$PWD/heapprof.so ./embedded_memory_benchmark tell-query 15 20 100 2 15
timeout 600 env HEAPPROF_OUT=/tmp/live-30 LD_PRELOAD=$PWD/heapprof.so ./embedded_memory_benchmark tell-query 30 20 100 2 30
python3 Tools/HeapProfiler/diff.py /tmp/live-15.<pid> /tmp/live-30.<pid>   # run from repo root (addr2line on ReleaseEmbedded/)
```

- Preload only the program you want to profile, not a wrapper such as
  `timeout`; that is why the command above is `timeout … env LD_PRELOAD=… ./program`.
- Known issue: it sometimes segfaults in longer runs (≥30 states); just
  retry. `diff.py` prints the call stacks whose live bytes grew most.
