// Memory benchmark for long incremental runs through the embedded C API
// (docs/INCREMENTAL_REASONING.md, task 5): cycles through many states on one
// handle and prints the process RSS (Linux: /proc/self/status) as it goes.
//
// Each state change retracts the previous state's facts and tells
// factsPerState new ClassAssertion(i<k>, Father) facts, with individual
// names drawn from a pool of poolSize names, so names get reused across
// states as in a planner's search.
//
// Modes:
//   tell-query  tell/retract, then queriesPerState Father/Person queries (the real use)
//   tell-only   tell/retract without queries (cost of revisions alone)
//   query-only  one fixed state, queries only (control: cost of repeated queries)
//
// Build: Tools/EmbeddedDriver/rebuild.sh (from repo root)
// Run:   ./embedded_memory_benchmark [mode] [states] [factsPerState] [poolSize] [queriesPerState] [reportEvery]
// Defaults: tell-query 500 20 100 2 50

#include "konclude_embedded.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char *kOntologyPath = "Tests/embedded-state-tbox-only.owl.xml";
const char *kFather = "http://example.org/test#Father";

const char *kQueries[] = {
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX ex: <http://example.org/test#>\n"
    "SELECT ?x WHERE { ?x rdf:type ex:Father . }",
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX ex: <http://example.org/test#>\n"
    "SELECT ?x WHERE { ?x rdf:type ex:Person . }"};

long readStatusKb(const char *field) {
  FILE *f = std::fopen("/proc/self/status", "r");
  if (!f) {
    return -1;
  }
  char line[256];
  long value = -1;
  size_t len = std::strlen(field);
  while (std::fgets(line, sizeof(line), f)) {
    if (std::strncmp(line, field, len) == 0) {
      value = std::atol(line + len);
      break;
    }
  }
  std::fclose(f);
  return value;
}

std::string individualIri(int idx) {
  return "http://example.org/test#i" + std::to_string(idx);
}

} // namespace

int main(int argc, char **argv) {
  std::string mode = argc >= 2 ? argv[1] : "tell-query";
  int states = argc >= 3 ? std::atoi(argv[2]) : 500;
  int factsPerState = argc >= 4 ? std::atoi(argv[3]) : 20;
  int poolSize = argc >= 5 ? std::atoi(argv[4]) : 100;
  int queriesPerState = argc >= 6 ? std::atoi(argv[5]) : 2;
  int reportEvery = argc >= 7 ? std::atoi(argv[6]) : 50;
  bool doChanges = mode != "query-only";
  bool doQueries = mode != "tell-only";
  if ((mode != "tell-query" && mode != "tell-only" && mode != "query-only") ||
      states <= 0 || factsPerState <= 0 || poolSize < factsPerState ||
      queriesPerState < 0 || reportEvery <= 0) {
    std::fprintf(stderr,
                 "Usage: %s [tell-query|tell-only|query-only] [states] "
                 "[factsPerState] [poolSize>=factsPerState] [queriesPerState] "
                 "[reportEvery]\n",
                 argv[0]);
    return 2;
  }

  KoncludeReasonerHandle handle = konclude_create_reasoner();
  if (!handle || !konclude_load_ontology_file(handle, kOntologyPath)) {
    std::fprintf(stderr, "setup failed: %s\n",
                 handle ? konclude_last_error(handle) : "");
    return 2;
  }

  std::printf("mode=%s states=%d factsPerState=%d poolSize=%d "
              "queriesPerState=%d\n",
              mode.c_str(), states, factsPerState, poolSize, queriesPerState);
  std::printf("%8s %12s %12s %10s\n", "state", "RSS_kB", "kB/state", "threads");
  long startRss = readStatusKb("VmRSS:");
  long lastRss = startRss;
  std::printf("%8d %12ld %12s %10ld\n", 0, startRss, "-",
              readStatusKb("Threads:"));

  std::vector<std::string> current;
  int failures = 0;
  unsigned int seed = 12345;
  for (int s = 1; s <= states; ++s) {
    if (doChanges || s == 1) {
      // next state: factsPerState distinct names from the pool
      std::vector<std::string> next;
      std::vector<char> used(poolSize, 0);
      while (static_cast<int>(next.size()) < factsPerState) {
        seed = seed * 1103515245u + 12345u;
        int idx = static_cast<int>((seed >> 8) % poolSize);
        if (!used[idx]) {
          used[idx] = 1;
          next.push_back(individualIri(idx));
        }
      }
      std::vector<KoncludeClassAssertion> tell, retract;
      for (const std::string &i : next) {
        tell.push_back({i.c_str(), kFather});
      }
      for (const std::string &i : current) {
        retract.push_back({i.c_str(), kFather});
      }
      if (!konclude_tell_and_retract_axioms(
              handle, tell.data(), static_cast<int>(tell.size()),
              retract.data(), static_cast<int>(retract.size()))) {
        ++failures;
      }
      current = next;
    }
    if (doQueries) {
      for (int q = 0; q < queriesPerState; ++q) {
        if (!konclude_execute_conjunctive_query(handle, kQueries[q % 2]) ||
            konclude_query_result_row_count(handle) != factsPerState) {
          ++failures;
        }
      }
    }
    if (s % reportEvery == 0) {
      long rss = readStatusKb("VmRSS:");
      std::printf("%8d %12ld %12.1f %10ld\n", s, rss,
                  static_cast<double>(rss - lastRss) / reportEvery,
                  readStatusKb("Threads:"));
      std::fflush(stdout);
      lastRss = rss;
    }
  }
  long endRss = readStatusKb("VmRSS:");
  std::printf("total growth: %ld kB over %d states = %.1f kB/state; "
              "%d failure(s)\n",
              endRss - startRss, states,
              static_cast<double>(endRss - startRss) / states, failures);
  konclude_destroy_reasoner(handle);
  return failures == 0 ? 0 : 1;
}
