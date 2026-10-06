// Handle-recycling memory benchmark for the embedded C API
// (docs/INCREMENTAL_REASONING.md, task 5): repeatedly creates a handle, uses
// it, and destroys it, and prints after each destroy what the process still
// holds -- RSS after malloc_trim(0) (returns freed memory of all malloc
// arenas; mallinfo2 would only cover the main arena) and the thread count.
// With a konclude_destroy_reasoner that frees everything, both stay flat.
//
// Workloads:
//   small    Tests/embedded-state-tbox-only.owl.xml, statesPerCycle state
//            changes of 20 Father facts, each followed by a Father query
//   roberts  Tests/roberts-family-full-D.owl.xml, one Father/hasChild query
//            (the embedded_cq_benchmark cycle)
//
// Build: Tools/EmbeddedDriver/rebuild.sh (from repo root)
// Run:   ./embedded_handle_cycle_benchmark [workload] [cycles] [statesPerCycle]
// Defaults: small 20 10

#include "konclude_embedded.h"

#include <malloc.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char *kSmallOntologyPath = "Tests/embedded-state-tbox-only.owl.xml";
const char *kSmallFather = "http://example.org/test#Father";
const char *kSmallQuery =
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX ex: <http://example.org/test#>\n"
    "SELECT ?x WHERE { ?x rdf:type ex:Father . }";
const int kSmallFactsPerState = 20;

const char *kRobertsOntologyPath = "Tests/roberts-family-full-D.owl.xml";
const char *kRobertsQuery =
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX fam: <http://www.co-ode.org/roberts/family-tree.owl#>\n"
    "SELECT ?x ?y WHERE { ?x rdf:type fam:Father . ?x fam:hasChild ?y . }";
const int kRobertsRows = 314;

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

long trimmedRssKb() {
  malloc_trim(0);
  return readStatusKb("VmRSS:");
}

std::string individualIri(int idx) {
  return "http://example.org/test#i" + std::to_string(idx);
}

// Runs the small workload on a fresh handle; returns the number of failures.
int runSmall(KoncludeReasonerHandle handle, int states) {
  int failures = 0;
  std::vector<std::string> current;
  for (int s = 0; s < states; ++s) {
    std::vector<std::string> next;
    for (int k = 0; k < kSmallFactsPerState; ++k) {
      next.push_back(individualIri((s * 7 + k) % 50));
    }
    std::vector<KoncludeClassAssertion> tell, retract;
    for (const std::string &i : next) {
      tell.push_back({i.c_str(), kSmallFather});
    }
    for (const std::string &i : current) {
      retract.push_back({i.c_str(), kSmallFather});
    }
    if (!konclude_tell_and_retract_axioms(
            handle, tell.data(), static_cast<int>(tell.size()), retract.data(),
            static_cast<int>(retract.size()))) {
      ++failures;
    }
    current = next;
    if (!konclude_execute_conjunctive_query(handle, kSmallQuery) ||
        konclude_query_result_row_count(handle) != kSmallFactsPerState) {
      ++failures;
    }
  }
  return failures;
}

} // namespace

int main(int argc, char **argv) {
  std::string workload = argc >= 2 ? argv[1] : "small";
  int cycles = argc >= 3 ? std::atoi(argv[2]) : 20;
  int statesPerCycle = argc >= 4 ? std::atoi(argv[3]) : 10;
  if ((workload != "small" && workload != "roberts") || cycles <= 0 ||
      statesPerCycle <= 0) {
    std::fprintf(stderr, "Usage: %s [small|roberts] [cycles] [statesPerCycle]\n",
                 argv[0]);
    return 2;
  }

  std::printf("workload=%s cycles=%d statesPerCycle=%d\n", workload.c_str(),
              cycles, statesPerCycle);
  std::printf("%6s %12s %10s\n", "cycle", "RSS_kB", "threads");
  std::printf("%6d %12ld %10ld\n", 0, trimmedRssKb(), readStatusKb("Threads:"));
  std::fflush(stdout);

  int failures = 0;
  long firstRss = -1, firstThreads = -1;
  for (int c = 1; c <= cycles; ++c) {
    KoncludeReasonerHandle handle = konclude_create_reasoner();
    const char *path =
        workload == "small" ? kSmallOntologyPath : kRobertsOntologyPath;
    if (!handle || !konclude_load_ontology_file(handle, path)) {
      std::fprintf(stderr, "cycle %d: setup failed: %s\n", c,
                   handle ? konclude_last_error(handle) : "");
      return 2;
    }
    if (workload == "small") {
      failures += runSmall(handle, statesPerCycle);
    } else if (!konclude_execute_conjunctive_query(handle, kRobertsQuery) ||
               konclude_query_result_row_count(handle) != kRobertsRows) {
      ++failures;
    }
    konclude_destroy_reasoner(handle);

    long rss = trimmedRssKb();
    long threads = readStatusKb("Threads:");
    if (c == 1) {
      firstRss = rss;
      firstThreads = threads;
    }
    std::printf("%6d %12ld %10ld\n", c, rss, threads);
    std::fflush(stdout);
  }
  long lastRss = trimmedRssKb();
  long lastThreads = readStatusKb("Threads:");
  if (cycles > 1) {
    std::printf("after cycle 1: %.1f kB RSS and %.2f threads kept per cycle; "
                "%d failure(s)\n",
                static_cast<double>(lastRss - firstRss) / (cycles - 1),
                static_cast<double>(lastThreads - firstThreads) / (cycles - 1),
                failures);
  }
  return failures == 0 ? 0 : 1;
}
