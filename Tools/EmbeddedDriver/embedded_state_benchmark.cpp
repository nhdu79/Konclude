// Benchmark driver for the embedded C API's "state" support (see
// Source/Control/Interface/Embedded/konclude_embedded.h,
// Tools/EmbeddedDriver/embedded_state_driver.cpp -- the correctness-focused
// sibling of this file -- and docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md's
// Decision 2).
//
// Measures how state-assertion and CQ-answering costs scale with the number
// of facts asserted into a single FD "state" (konclude_state_begin -> N x
// konclude_state_assert_class_fact -> M x
// konclude_execute_conjunctive_query), which is the actual FD usage shape:
// one reasoner instance, one TBox load, then repeatedly begin a state,
// assert its facts, and answer several CQs against that state before moving
// on to the next one. Unlike embedded_cq_benchmark.cpp (which measures N
// independent fresh-instance load+query cycles), this benchmark keeps ONE
// handle and ONE TBox load for the whole run and varies the number of facts
// per state.
//
// For each fact count in step, 2*step, ..., maxFacts:
//   - begin a fresh state
//   - assert that many ClassAssertion(indI, Father) facts, timed as one
//     batch per repeat (assertStats)
//   - run one CQ for Father (rowCount must equal the fact count) -- this is
//     the FIRST query against the state, so its cost includes the lazy
//     Install (see Decision 2) as well as the query itself (firstQueryStats)
//   - run (queriesPerState - 1) further CQs, alternating Father/Person,
//     against the SAME state -- these do NOT pay the install cost, so
//     comparing their mean against the first query's isolates it
//     (repeatQueryStats)
//
// Build (from repo root, after building KoncludeEmbedded.pro -- same
// library embedded_state_driver.cpp uses):
//   g++ -std=c++11 -O2 -I Source/Control/Interface/Embedded \
//       Tools/EmbeddedDriver/embedded_state_benchmark.cpp \
//       -L ReleaseEmbedded -lKonclude -Wl,-rpath,ReleaseEmbedded \
//       -o embedded_state_benchmark
// Run:
//   ./embedded_state_benchmark [maxFacts] [step] [repeats] [queriesPerState]
// All arguments optional. Defaults: maxFacts=200, step=10, repeats=5,
// queriesPerState=5. Uses Tests/embedded-state-tbox-only.owl.xml
// (SubClassOf(Father, Person)) as the TBox.

#include "konclude_embedded.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

const char *kOntologyPath = "Tests/embedded-state-tbox-only.owl.xml";
const char *kClassFather = "http://example.org/test#Father";

const char *kQueryFathers =
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX ex: <http://example.org/test#>\n"
    "SELECT ?x WHERE { ?x rdf:type ex:Father . }";

const char *kQueryPersons =
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX ex: <http://example.org/test#>\n"
    "SELECT ?x WHERE { ?x rdf:type ex:Person . }";

std::string individualIri(int idx) {
  return "http://example.org/test#ind" + std::to_string(idx);
}

typedef std::chrono::steady_clock Clock;
typedef std::chrono::duration<double, std::milli> MillisD;

double elapsedMs(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration_cast<MillisD>(end - start).count();
}

struct Stats {
  double totalMs = 0.0;
  double minMs = -1.0;
  double maxMs = 0.0;
  int count = 0;

  void add(double ms) {
    totalMs += ms;
    if (minMs < 0.0 || ms < minMs) minMs = ms;
    if (ms > maxMs) maxMs = ms;
    ++count;
  }

  double meanMs() const { return count > 0 ? totalMs / count : 0.0; }
};

// Runs `repeats` independent (begin state -> assert factCount facts -> run
// queriesPerState CQs) cycles for one fact count and accumulates timing into
// the three Stats out-params. Returns the number of cycles that failed a
// call outright or returned a mismatched row count.
int runForSize(KoncludeReasonerHandle handle, int factCount, int repeats,
               int queriesPerState, Stats &assertStats, Stats &firstQueryStats,
               Stats &repeatQueryStats) {
  int problems = 0;

  for (int r = 0; r < repeats; ++r) {
    if (!konclude_state_begin(handle)) {
      std::fprintf(stderr, "facts=%d repeat=%d: state_begin failed: %s\n",
                   factCount, r, konclude_last_error(handle));
      ++problems;
      continue;
    }

    Clock::time_point t0 = Clock::now();
    bool assertFailed = false;
    for (int i = 0; i < factCount; ++i) {
      if (!konclude_state_assert_class_fact(
              handle, individualIri(i).c_str(), kClassFather)) {
        std::fprintf(stderr, "facts=%d repeat=%d: assert %d failed: %s\n",
                     factCount, r, i, konclude_last_error(handle));
        assertFailed = true;
        break;
      }
    }
    Clock::time_point t1 = Clock::now();
    if (assertFailed) {
      ++problems;
      continue;
    }
    assertStats.add(elapsedMs(t0, t1));

    Clock::time_point tq0 = Clock::now();
    if (!konclude_execute_conjunctive_query(handle, kQueryFathers)) {
      std::fprintf(stderr, "facts=%d repeat=%d: first query failed: %s\n",
                   factCount, r, konclude_last_error(handle));
      ++problems;
      continue;
    }
    Clock::time_point tq1 = Clock::now();
    firstQueryStats.add(elapsedMs(tq0, tq1));
    if (konclude_query_result_row_count(handle) != factCount) {
      std::fprintf(stderr,
                   "facts=%d repeat=%d: first query rowCount=%d, expected %d\n",
                   factCount, r, konclude_query_result_row_count(handle),
                   factCount);
      ++problems;
    }

    for (int q = 1; q < queriesPerState; ++q) {
      const char *query = (q % 2 == 0) ? kQueryFathers : kQueryPersons;
      Clock::time_point tr0 = Clock::now();
      if (!konclude_execute_conjunctive_query(handle, query)) {
        std::fprintf(stderr, "facts=%d repeat=%d query=%d: query failed: %s\n",
                     factCount, r, q, konclude_last_error(handle));
        ++problems;
        continue;
      }
      Clock::time_point tr1 = Clock::now();
      repeatQueryStats.add(elapsedMs(tr0, tr1));
      if (konclude_query_result_row_count(handle) != factCount) {
        std::fprintf(
            stderr, "facts=%d repeat=%d query=%d: rowCount=%d, expected %d\n",
            factCount, r, q, konclude_query_result_row_count(handle),
            factCount);
        ++problems;
      }
    }
  }

  return problems;
}

} // namespace

int main(int argc, char **argv) {
  int maxFacts = 200;
  int step = 10;
  int repeats = 5;
  int queriesPerState = 5;

  if (argc >= 2) maxFacts = std::atoi(argv[1]);
  if (argc >= 3) step = std::atoi(argv[2]);
  if (argc >= 4) repeats = std::atoi(argv[3]);
  if (argc >= 5) queriesPerState = std::atoi(argv[4]);

  if (maxFacts <= 0 || step <= 0 || repeats <= 0 || queriesPerState <= 0) {
    std::fprintf(stderr,
                 "Usage: %s [maxFacts>0] [step>0] [repeats>0] "
                 "[queriesPerState>0]\n",
                 argv[0]);
    return 2;
  }

  std::printf("Benchmarking state scaling: facts = %d..%d step %d, "
              "%d repeats/size, %d queries/state\n",
              step, maxFacts, step, repeats, queriesPerState);
  std::printf("Using TBox: %s\n\n", kOntologyPath);

  KoncludeReasonerHandle handle = konclude_create_reasoner();
  if (!handle) {
    std::fprintf(stderr, "konclude_create_reasoner failed\n");
    return 2;
  }
  if (!konclude_load_ontology_file(handle, kOntologyPath)) {
    std::fprintf(stderr, "load failed: %s\n", konclude_last_error(handle));
    konclude_destroy_reasoner(handle);
    return 2;
  }

  std::printf(
      "%8s  %20s  %14s  %20s  %20s\n", "facts", "assert total(ms mean)",
      "assert(ms/fact)", "first-query(ms mean)", "repeat-query(ms mean)");

  int totalProblems = 0;
  Clock::time_point benchStart = Clock::now();

  for (int factCount = step; factCount <= maxFacts; factCount += step) {
    Stats assertStats, firstQueryStats, repeatQueryStats;
    totalProblems += runForSize(handle, factCount, repeats, queriesPerState,
                                 assertStats, firstQueryStats,
                                 repeatQueryStats);

    double assertMsPerFact =
        factCount > 0 ? assertStats.meanMs() / factCount : 0.0;
    std::printf("%8d  %20.3f  %14.4f  %20.3f  %20.3f\n", factCount,
                assertStats.meanMs(), assertMsPerFact,
                firstQueryStats.meanMs(), repeatQueryStats.meanMs());
  }

  Clock::time_point benchEnd = Clock::now();

  konclude_destroy_reasoner(handle);

  std::printf("\nWall clock: %.1f ms (%.3f s)\n",
              elapsedMs(benchStart, benchEnd),
              elapsedMs(benchStart, benchEnd) / 1000.0);
  std::printf("%d problem(s) encountered (failed calls or mismatched row "
              "counts)\n",
              totalProblems);

  return totalProblems == 0 ? 0 : 1;
}
