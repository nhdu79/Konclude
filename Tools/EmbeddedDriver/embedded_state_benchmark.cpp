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
// Like embedded_state_driver.cpp, each repeat alternates between two
// distinct states so the benchmark also exercises (and verifies) the same
// three properties the driver checks, at every fact count instead of just
// once:
//   1. An asserted fact is visible to a query, and reasoning ran over it --
//      the Person query on State A's Father individuals only succeeds
//      because of SubClassOf(Father, Person), not literal triple matching.
//   2. Repeated queries against the SAME state see a stable, correct result
//      set (checked by full set comparison, not just row count).
//   3. Starting a NEW state discards the previous state's facts: State B
//      uses a disjoint set of individuals and never asserts Father, so its
//      Father query must come back completely empty -- any hit means a
//      leak forward from State A.
//
// For each fact count in step, 2*step, ..., maxFacts, and for each of
// `repeats` iterations:
//   - State A: begin a fresh state, assert factCount ClassAssertion(a<i>,
//     Father) facts (timed into assertStats), then run queriesPerState CQs
//     against it (alternating Father/Person), verifying each result set is
//     exactly the "a" individuals. The first such query's timing is kept
//     separate (firstQueryStats) since it includes the lazy Install cost
//     (see Decision 2); the rest go into repeatQueryStats.
//   - State B: begin ANOTHER fresh state, assert factCount
//     ClassAssertion(b<i>, Person) facts -- a disjoint individual namespace,
//     and never Father -- then run queriesPerState CQs verifying the Father
//     query is empty (no leak from State A) and the Person query is exactly
//     the "b" individuals. Timed into the same assertStats/firstQueryStats/
//     repeatQueryStats buckets as State A so the printed table stays
//     comparable across fact counts.
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
#include <set>
#include <string>
#include <vector>

namespace {

const char *kOntologyPath = "Tests/embedded-state-tbox-only.owl.xml";
const char *kClassFather = "http://example.org/test#Father";
const char *kClassPerson = "http://example.org/test#Person";

const char *kQueryFathers =
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX ex: <http://example.org/test#>\n"
    "SELECT ?x WHERE { ?x rdf:type ex:Father . }";

const char *kQueryPersons =
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX ex: <http://example.org/test#>\n"
    "SELECT ?x WHERE { ?x rdf:type ex:Person . }";

// State A uses the "a" namespace and asserts Father; State B uses the
// disjoint "b" namespace and asserts Person only -- so any "a" individual
// (or any Father result at all) showing up while State B is current proves
// a leak.
std::string individualIri(const char *prefix, int idx) {
  return std::string("http://example.org/test#") + prefix +
         std::to_string(idx);
}

std::set<std::string> expectedIndividuals(const char *prefix, int count) {
  std::set<std::string> expected;
  for (int i = 0; i < count; ++i) expected.insert(individualIri(prefix, i));
  return expected;
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

// Asserts factCount ClassAssertion(individualIri(prefix,i), className)
// facts into the current (already-begun) state, timed as one batch into
// assertStats.
bool assertFacts(KoncludeReasonerHandle handle, const char *prefix,
                  const char *className, int factCount, Stats &assertStats,
                  const char *stateLabel) {
  Clock::time_point t0 = Clock::now();
  for (int i = 0; i < factCount; ++i) {
    if (!konclude_state_assert_class_fact(
            handle, individualIri(prefix, i).c_str(), className)) {
      std::fprintf(stderr, "%s: assert %d failed: %s\n", stateLabel, i,
                   konclude_last_error(handle));
      return false;
    }
  }
  Clock::time_point t1 = Clock::now();
  assertStats.add(elapsedMs(t0, t1));
  return true;
}

// Runs one conjunctive query, times it into `stats` (if non-null) covering
// only the konclude_execute_conjunctive_query call itself, and returns the
// bound ?x values as a set for correctness comparison by the caller.
std::set<std::string> runTimedQuery(KoncludeReasonerHandle handle,
                                     const char *query, Stats *stats,
                                     bool &ok) {
  Clock::time_point t0 = Clock::now();
  ok = konclude_execute_conjunctive_query(handle, query);
  Clock::time_point t1 = Clock::now();

  std::set<std::string> results;
  if (!ok) return results;
  if (stats) stats->add(elapsedMs(t0, t1));

  int rowCount = konclude_query_result_row_count(handle);
  for (int row = 0; row < rowCount; ++row) {
    results.insert(konclude_query_result_binding(handle, row, 0));
  }
  return results;
}

// Runs queriesPerState CQs against the current state (alternating
// Father/Person, same schedule as before), checking each result set exactly
// matches `expectedFathers`/`expectedPersons`. The first query's timing goes
// into firstQueryStats (it pays the lazy Install cost), the rest into
// repeatQueryStats. Returns the number of problems encountered.
int runQueriesAgainstState(KoncludeReasonerHandle handle, int factCount,
                            int repeatIdx, const char *stateLabel,
                            int queriesPerState,
                            const std::set<std::string> &expectedFathers,
                            const std::set<std::string> &expectedPersons,
                            Stats &firstQueryStats, Stats &repeatQueryStats) {
  int problems = 0;
  bool ok;

  std::set<std::string> first = runTimedQuery(handle, kQueryFathers,
                                               &firstQueryStats, ok);
  if (!ok) {
    std::fprintf(stderr, "facts=%d repeat=%d %s: first query failed: %s\n",
                 factCount, repeatIdx, stateLabel,
                 konclude_last_error(handle));
    ++problems;
  } else if (first != expectedFathers) {
    std::fprintf(stderr,
                 "facts=%d repeat=%d %s: first Father query returned %zu "
                 "individuals, expected %zu (mismatch -- possible leak or "
                 "missing fact)\n",
                 factCount, repeatIdx, stateLabel, first.size(),
                 expectedFathers.size());
    ++problems;
  }

  for (int q = 1; q < queriesPerState; ++q) {
    bool wantFathers = (q % 2 == 0);
    const char *query = wantFathers ? kQueryFathers : kQueryPersons;
    const std::set<std::string> &expected =
        wantFathers ? expectedFathers : expectedPersons;

    std::set<std::string> result =
        runTimedQuery(handle, query, &repeatQueryStats, ok);
    if (!ok) {
      std::fprintf(stderr, "facts=%d repeat=%d %s query=%d: query failed: %s\n",
                   factCount, repeatIdx, stateLabel, q,
                   konclude_last_error(handle));
      ++problems;
      continue;
    }
    if (result != expected) {
      std::fprintf(stderr,
                   "facts=%d repeat=%d %s query=%d (%s): got %zu "
                   "individuals, expected %zu\n",
                   factCount, repeatIdx, stateLabel, q,
                   wantFathers ? "Father" : "Person", result.size(),
                   expected.size());
      ++problems;
    }
  }

  return problems;
}

// Runs one (State A -> State B) pair for one fact count / repeat, timing
// asserts and queries from both states into the same Stats buckets. See the
// file header for what each state asserts and checks.
int runStatePair(KoncludeReasonerHandle handle, int factCount, int repeatIdx,
                  int queriesPerState, Stats &assertStats,
                  Stats &firstQueryStats, Stats &repeatQueryStats) {
  int problems = 0;

  // ---- State A: factCount "a<i>" individuals asserted as Father. ----
  if (!konclude_state_begin(handle)) {
    std::fprintf(stderr, "facts=%d repeat=%d state A: state_begin failed: %s\n",
                 factCount, repeatIdx, konclude_last_error(handle));
    return problems + 1;
  }
  if (!assertFacts(handle, "a", kClassFather, factCount, assertStats,
                    "state A")) {
    return problems + 1;
  }

  // Father implies Person (SubClassOf(Father, Person) in the TBox), so in
  // State A both queries should return exactly the "a" individuals.
  std::set<std::string> expectedA = expectedIndividuals("a", factCount);
  problems += runQueriesAgainstState(handle, factCount, repeatIdx, "state A",
                                      queriesPerState, expectedA, expectedA,
                                      firstQueryStats, repeatQueryStats);

  // ---- State B: fresh state, disjoint "b<i>" individuals, Person only. ----
  // Never asserts Father, so any Father-query hit here -- especially any
  // "a" individual -- proves State A's facts leaked forward.
  if (!konclude_state_begin(handle)) {
    std::fprintf(stderr, "facts=%d repeat=%d state B: state_begin failed: %s\n",
                 factCount, repeatIdx, konclude_last_error(handle));
    return problems + 1;
  }
  if (!assertFacts(handle, "b", kClassPerson, factCount, assertStats,
                    "state B")) {
    return problems + 1;
  }

  std::set<std::string> expectedFathersB;  // must stay empty
  std::set<std::string> expectedPersonsB = expectedIndividuals("b", factCount);
  problems += runQueriesAgainstState(handle, factCount, repeatIdx, "state B",
                                      queriesPerState, expectedFathersB,
                                      expectedPersonsB, firstQueryStats,
                                      repeatQueryStats);

  return problems;
}

// Runs `repeats` (State A -> State B) cycles for one fact count and
// accumulates timing into the three Stats out-params. Returns the number of
// problems (failed calls or incorrect/leaked result sets) encountered.
int runForSize(KoncludeReasonerHandle handle, int factCount, int repeats,
               int queriesPerState, Stats &assertStats, Stats &firstQueryStats,
               Stats &repeatQueryStats) {
  int problems = 0;
  for (int r = 0; r < repeats; ++r) {
    problems += runStatePair(handle, factCount, r, queriesPerState,
                              assertStats, firstQueryStats, repeatQueryStats);
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
              "%d repeats/size (each repeat = State A + State B), "
              "%d queries/state\n",
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

    // Each repeat asserts factCount facts twice (State A, then State B), so
    // assertStats.count == 2 * repeats batches of factCount asserts each.
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
  std::printf("%d problem(s) encountered (failed calls, incorrect result "
              "sets, or leaked facts)\n",
              totalProblems);

  return totalProblems == 0 ? 0 : 1;
}
