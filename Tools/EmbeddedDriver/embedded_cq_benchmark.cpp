// Benchmark driver for the embedded C API's conjunctive-query support (see
// Source/Control/Interface/Embedded/konclude_embedded.h and
// Tools/EmbeddedDriver/embedded_cq_driver.cpp, the correctness-focused
// sibling of this file).
//
// Measures the cost of N independent (ABox load + conjunctive query answer)
// cycles, for one or more named scenarios (currently: the Roberts family
// tree ontology, and the 22x22 robot grid ontology). Each iteration is
// fully independent: a fresh reasoner instance is created, the SAME
// ontology file is loaded again from scratch (the C API's
// konclude_load_ontology_file may only be called once per handle -- see its
// doc comment -- so "load the ABox again" means a new instance per
// iteration, not a second load call on one instance), a state is begun with
// no asserted facts (konclude_execute_conjunctive_query() requires a
// current state; an empty one just queries the loaded ABox as-is), and the
// SAME SPARQL query text is answered again -- konclude_execute_conjunctive_query()
// always parses its query-text argument and builds a fresh CQuery from
// scratch internally (see CEmbeddedReasoner::executeConjunctiveQuery()), so
// passing the same query string each iteration already satisfies "full
// construction as a new one" without any extra code here.
//
// Build (from repo root, after building KoncludeEmbedded.pro -- same
// library embedded_cq_driver.cpp uses):
//   g++ -std=c++11 -O2 -I Source/Control/Interface/Embedded \
//       Tools/EmbeddedDriver/embedded_cq_benchmark.cpp \
//       -L ReleaseEmbedded -lKonclude -Wl,-rpath,ReleaseEmbedded \
//       -o embedded_cq_benchmark
// Run:
//   ./embedded_cq_benchmark [iterations] [scenario]
// `iterations` defaults to 1000. `scenario` selects which ontology/query
// pair(s) to run: "roberts-family", "robot-grid", or "all" (default).

#include "konclude_embedded.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

// Roberts family tree: Father(x), hasChild(x,y) -- 314 expected (x,y) pairs,
// see docs/EMBEDDED_CQ_DRIVER.md / Tests/roberts-family-simple-cq-test.sparql.
const char *kRobertsOntologyPath = "Tests/roberts-family-full-D.owl.xml";
const char *kRobertsQuery =
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX fam: <http://www.co-ode.org/roberts/family-tree.owl#>\n"
    "SELECT ?x ?y WHERE { ?x rdf:type fam:Father . ?x fam:hasChild ?y . }";
const int kRobertsExpectedRows = 314;

// Robot grid (Tests/robot22.owl.xml): the single asserted individual
// `robot` is LeftOf10/BelowOf10, and LeftOf10 (resp. BelowOf10) is a
// subclass of LeftOf11...LeftOf15 (resp. BelowOf11...BelowOf15) in the
// ontology's TBox, so robot is inferred to be LeftOf15 and BelowOf15 too --
// exactly 1 expected answer (robot itself). The ontology has no `#`
// fragment convention for its classes (full absolute IRIs throughout), so
// the query uses a PREFIX for that namespace rather than the usual
// `#Foo`-style shorthand.
const char *kRobotGridOntologyPath = "Tests/robot22.owl.xml";
const char *kRobotGridQuery =
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX grid: <http://www.semanticweb.org/anon/ontologies/2021/3/>\n"
    "SELECT ?x WHERE { ?x rdf:type grid:LeftOf15 . ?x rdf:type grid:BelowOf15 . }";
const int kRobotGridExpectedRows = 1;

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

void printStats(const char *label, const Stats &s) {
  std::printf("  %-12s total=%9.1f ms  mean=%7.3f ms  min=%7.3f ms  max=%9.3f ms\n",
              label, s.totalMs, s.meanMs(), s.minMs, s.maxMs);
}

// Runs `iterations` independent (create -> load -> query -> destroy)
// cycles for one scenario and prints its stats. Returns true iff every
// cycle succeeded and returned exactly `expectedRowCount` rows.
bool runScenario(const char *scenarioName, const std::string &ontologyPath,
                  const char *query, int expectedRowCount, int iterations) {
  std::printf("\n=== %s ===\n", scenarioName);
  std::printf("Benchmarking %d independent (load ABox + answer CQ) cycles against %s\n",
              iterations, ontologyPath.c_str());

  Stats createStats, loadStats, queryStats, destroyStats, totalStats;
  int mismatches = 0;
  int failures = 0;

  const int progressEvery = std::max(1, iterations / 20);

  Clock::time_point benchStart = Clock::now();

  for (int i = 0; i < iterations; ++i) {
    Clock::time_point t0 = Clock::now();

    KoncludeReasonerHandle handle = konclude_create_reasoner();
    Clock::time_point t1 = Clock::now();
    if (!handle) {
      std::fprintf(stderr, "%s iteration %d: konclude_create_reasoner failed\n", scenarioName, i);
      ++failures;
      continue;
    }

    if (!konclude_load_ontology_file(handle, ontologyPath.c_str())) {
      std::fprintf(stderr, "%s iteration %d: load failed: %s\n", scenarioName, i, konclude_last_error(handle));
      konclude_destroy_reasoner(handle);
      ++failures;
      continue;
    }

    // konclude_execute_conjunctive_query() requires a current state (see
    // konclude_embedded.h) -- konclude_state_begin() with no asserted facts
    // just wraps the loaded ontology as-is, matching this benchmark's
    // "query the ABox as loaded" scenarios.
    if (!konclude_state_begin(handle)) {
      std::fprintf(stderr, "%s iteration %d: state_begin failed: %s\n", scenarioName, i, konclude_last_error(handle));
      konclude_destroy_reasoner(handle);
      ++failures;
      continue;
    }
    Clock::time_point t2 = Clock::now();

    if (!konclude_execute_conjunctive_query(handle, query)) {
      std::fprintf(stderr, "%s iteration %d: query failed: %s\n", scenarioName, i, konclude_last_error(handle));
      konclude_destroy_reasoner(handle);
      ++failures;
      continue;
    }
    Clock::time_point t3 = Clock::now();

    int rowCount = konclude_query_result_row_count(handle);
    if (rowCount != expectedRowCount) {
      ++mismatches;
    }

    konclude_destroy_reasoner(handle);
    Clock::time_point t4 = Clock::now();

    createStats.add(elapsedMs(t0, t1));
    loadStats.add(elapsedMs(t1, t2));
    queryStats.add(elapsedMs(t2, t3));
    destroyStats.add(elapsedMs(t3, t4));
    totalStats.add(elapsedMs(t0, t4));

    if ((i + 1) % progressEvery == 0 || i + 1 == iterations) {
      std::printf("  [%d/%d] running mean/cycle so far: %.3f ms\n", i + 1, iterations, totalStats.meanMs());
    }
  }

  Clock::time_point benchEnd = Clock::now();

  std::printf("\nDone: %d cycles attempted, %d failed, %d cycles with row count != expected (%d)\n",
              iterations, failures, mismatches, expectedRowCount);
  std::printf("Wall clock for this scenario: %.1f ms (%.3f s)\n",
              elapsedMs(benchStart, benchEnd), elapsedMs(benchStart, benchEnd) / 1000.0);
  std::printf("Per-phase breakdown (ms):\n");
  printStats("create", createStats);
  printStats("load+state", loadStats);
  printStats("query", queryStats);
  printStats("destroy", destroyStats);
  printStats("total/cycle", totalStats);

  return failures == 0 && mismatches == 0;
}

} // namespace

int main(int argc, char **argv) {
  int iterations = 1000;
  std::string scenario = "all";

  if (argc >= 2) iterations = std::atoi(argv[1]);
  if (argc >= 3) scenario = argv[2];
  if (iterations <= 0) {
    std::fprintf(stderr, "Usage: %s [iterations>0] [roberts-family|robot-grid|all]\n", argv[0]);
    return 2;
  }

  bool ok = true;
  bool ranAny = false;

  if (scenario == "roberts-family" || scenario == "all") {
    ranAny = true;
    ok = runScenario("roberts-family", kRobertsOntologyPath, kRobertsQuery, kRobertsExpectedRows, iterations) && ok;
  }
  if (scenario == "robot-grid" || scenario == "all") {
    ranAny = true;
    ok = runScenario("robot-grid", kRobotGridOntologyPath, kRobotGridQuery, kRobotGridExpectedRows, iterations) && ok;
  }
  if (!ranAny) {
    std::fprintf(stderr, "Unknown scenario '%s'; expected roberts-family, robot-grid, or all\n", scenario.c_str());
    return 2;
  }

  return ok ? 0 : 1;
}
