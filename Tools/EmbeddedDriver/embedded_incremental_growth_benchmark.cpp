// Incremental-growth benchmark for the embedded C API
// (docs/INCREMENTAL_REASONING.md, task 5): one handle goes through many small
// state changes, as in a planner's search path. Each change tells 2 new
// Father facts and, every second step, retracts an older one, so the state
// grows by 1-2 axioms per change. After each change, queriesPerState Father
// queries are answered (checked against the expected row count).
//
// Every reportEvery states it prints the RSS after malloc_trim(0) and the mean
// time per state (change + queries) of that block. At the end it measures what
// recreating the handle costs: destroy the old handle, then create, load, tell
// the whole current state in one call, and answer one query. (Two handles
// alive at the same time are not supported, see CEmbeddedReasoner.)
//
// Build: Tools/EmbeddedDriver/rebuild.sh (from repo root)
// With recreateEvery > 0, the handle is destroyed and recreated (load + tell
// the current state) after every recreateEvery states, counted in ms/state.
//
// Run:   ./embedded_incremental_growth_benchmark [states] [queriesPerState] [reportEvery] [recreateEvery]
// Defaults: 1000 2 100 0

#include "konclude_embedded.h"

#include <malloc.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace {

const char *kOntologyPath = "Tests/embedded-state-tbox-only.owl.xml";
const char *kFather = "http://example.org/test#Father";
const char *kQuery =
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX ex: <http://example.org/test#>\n"
    "SELECT ?x WHERE { ?x rdf:type ex:Father . }";

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

double msSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

std::string individualIri(int idx) {
  return "http://example.org/test#g" + std::to_string(idx);
}

// Returns true iff the Father query answers exactly expectedRows rows.
bool queryMatches(KoncludeReasonerHandle handle, int expectedRows) {
  return konclude_execute_conjunctive_query(handle, kQuery) &&
         konclude_query_result_row_count(handle) == expectedRows;
}

// Creates a handle, loads the TBox and tells the given state (no query yet);
// returns nullptr on failure.
KoncludeReasonerHandle createWithState(const std::deque<std::string> &state) {
  KoncludeReasonerHandle handle = konclude_create_reasoner();
  if (!handle || !konclude_load_ontology_file(handle, kOntologyPath)) {
    return nullptr;
  }
  if (!state.empty()) {
    std::vector<KoncludeClassAssertion> all;
    for (const std::string &i : state) {
      all.push_back({i.c_str(), kFather});
    }
    if (!konclude_tell_and_retract_axioms(handle, all.data(),
                                          static_cast<int>(all.size()), nullptr,
                                          0)) {
      konclude_destroy_reasoner(handle);
      return nullptr;
    }
  }
  return handle;
}

} // namespace

int main(int argc, char **argv) {
  int states = argc >= 2 ? std::atoi(argv[1]) : 1000;
  int queriesPerState = argc >= 3 ? std::atoi(argv[2]) : 2;
  int reportEvery = argc >= 4 ? std::atoi(argv[3]) : 100;
  int recreateEvery = argc >= 5 ? std::atoi(argv[4]) : 0;
  if (states <= 0 || queriesPerState < 0 || reportEvery <= 0 ||
      recreateEvery < 0) {
    std::fprintf(stderr,
                 "Usage: %s [states] [queriesPerState] [reportEvery] "
                 "[recreateEvery]\n",
                 argv[0]);
    return 2;
  }

  KoncludeReasonerHandle handle = konclude_create_reasoner();
  if (!handle || !konclude_load_ontology_file(handle, kOntologyPath)) {
    std::fprintf(stderr, "setup failed: %s\n",
                 handle ? konclude_last_error(handle) : "");
    return 2;
  }

  std::printf("states=%d queriesPerState=%d recreateEvery=%d (+2 facts per "
              "change, -1 every second change)\n",
              states, queriesPerState, recreateEvery);
  std::printf("%8s %8s %12s %12s %12s\n", "state", "facts", "RSS_kB",
              "kB/state", "ms/state");
  long startRss = trimmedRssKb();
  long lastRss = startRss;
  std::printf("%8d %8d %12ld %12s %12s\n", 0, 0, startRss, "-", "-");
  std::fflush(stdout);

  std::deque<std::string> current;
  int nextName = 0;
  int failures = 0;
  auto blockStart = std::chrono::steady_clock::now();
  for (int s = 1; s <= states; ++s) {
    if (recreateEvery > 0 && s > 1 && (s - 1) % recreateEvery == 0) {
      // two handles must not be alive at the same time
      konclude_destroy_reasoner(handle);
      handle = createWithState(current);
      if (!handle) {
        std::fprintf(stderr, "recreating the handle failed at state %d\n", s);
        return 2;
      }
    }
    std::vector<std::string> told = {individualIri(nextName),
                                     individualIri(nextName + 1)};
    nextName += 2;
    std::vector<std::string> retracted;
    if (s % 2 == 0 && !current.empty()) {
      retracted.push_back(current.front());
      current.pop_front();
    }
    std::vector<KoncludeClassAssertion> tell, retract;
    for (const std::string &i : told) {
      tell.push_back({i.c_str(), kFather});
      current.push_back(i);
    }
    for (const std::string &i : retracted) {
      retract.push_back({i.c_str(), kFather});
    }
    if (!konclude_tell_and_retract_axioms(
            handle, tell.data(), static_cast<int>(tell.size()), retract.data(),
            static_cast<int>(retract.size()))) {
      ++failures;
    }
    for (int q = 0; q < queriesPerState; ++q) {
      if (!queryMatches(handle, static_cast<int>(current.size()))) {
        ++failures;
      }
    }
    if (s % reportEvery == 0) {
      double msPerState = msSince(blockStart) / reportEvery;
      long rss = trimmedRssKb();
      std::printf("%8d %8zu %12ld %12.1f %12.2f\n", s, current.size(), rss,
                  static_cast<double>(rss - lastRss) / reportEvery, msPerState);
      std::fflush(stdout);
      lastRss = rss;
      blockStart = std::chrono::steady_clock::now();
    }
  }
  long endRss = trimmedRssKb();
  std::printf("total growth: %ld kB over %d states = %.1f kB/state; %d "
              "failure(s)\n",
              endRss - startRss, states,
              static_cast<double>(endRss - startRss) / states, failures);
  std::fflush(stdout);

  // Cost of recreating the handle with the current state.
  auto destroyStart = std::chrono::steady_clock::now();
  konclude_destroy_reasoner(handle);
  double destroyMs = msSince(destroyStart);
  long afterDestroyRss = trimmedRssKb();
  // all malloc arenas: "system bytes" held vs. "in use bytes" (stderr)
  std::fprintf(stderr, "malloc_stats after destroying the handle:\n");
  malloc_stats();
  auto recreateStart = std::chrono::steady_clock::now();
  KoncludeReasonerHandle fresh = konclude_create_reasoner();
  bool loaded = fresh && konclude_load_ontology_file(fresh, kOntologyPath);
  double loadMs = msSince(recreateStart);
  std::vector<KoncludeClassAssertion> all;
  for (const std::string &i : current) {
    all.push_back({i.c_str(), kFather});
  }
  auto tellStart = std::chrono::steady_clock::now();
  bool told = loaded && konclude_tell_and_retract_axioms(
                            fresh, all.data(), static_cast<int>(all.size()),
                            nullptr, 0);
  bool answered = told && queryMatches(fresh, static_cast<int>(current.size()));
  double tellQueryMs = msSince(tellStart);
  long afterRecreateRss = trimmedRssKb();
  if (!answered) {
    ++failures;
  }
  std::printf("recreate: destroy %.1f ms (RSS then %ld kB), create+load %.1f "
              "ms, tell %zu facts + first query %.1f ms; RSS afterwards %ld kB "
              "(start %ld kB); %d failure(s)\n",
              destroyMs, afterDestroyRss, loadMs, current.size(), tellQueryMs,
              afterRecreateRss, startRss, failures);
  konclude_destroy_reasoner(fresh);
  return failures == 0 ? 0 : 1;
}
