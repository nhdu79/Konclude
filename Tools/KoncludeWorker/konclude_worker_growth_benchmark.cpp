// Growth benchmark through KoncludeWorkerClient: the workload of
// Tools/EmbeddedDriver/embedded_incremental_growth_benchmark.cpp (each change
// tells 2 new Father facts and retracts 1 every second step; 2 queries per
// state), run in a konclude_worker restarted every restartEvery changes.
// Reports the worker's RSS and the mean time per state (incl. restarts) for
// each block of reportEvery states.
//
// Build: Tools/KoncludeWorker/build.sh (from repo root)
// Run:   ./konclude_worker_growth_benchmark [states] [restartEvery] [reportEvery]
// Defaults: 1000 100 100

#include "konclude_worker_client.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
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

std::string individualIri(int idx) {
  return "http://example.org/test#g" + std::to_string(idx);
}

} // namespace

int main(int argc, char **argv) {
  int states = argc >= 2 ? std::atoi(argv[1]) : 1000;
  int restartEvery = argc >= 3 ? std::atoi(argv[2]) : 100;
  int reportEvery = argc >= 4 ? std::atoi(argv[3]) : 100;
  if (states <= 0 || restartEvery < 0 || reportEvery <= 0) {
    std::fprintf(stderr, "Usage: %s [states] [restartEvery] [reportEvery]\n", argv[0]);
    return 2;
  }

  KoncludeWorkerClient client("./konclude_worker", restartEvery);
  if (!client.ok() || !client.loadOntology(kOntologyPath)) {
    std::fprintf(stderr, "worker setup failed: %s\n", client.lastError());
    return 2;
  }
  std::printf("states=%d restartEvery=%d (+2 facts per change, -1 every second change)\n",
              states, restartEvery);
  std::printf("%8s %8s %14s %14s %12s %10s\n", "state", "facts", "worker_RSS_kB",
              "max_RSS_kB", "ms/state", "restarts");

  std::deque<std::string> current;
  int nextName = 0, failures = 0;
  long maxRss = 0;
  double totalMs = 0;
  auto blockStart = std::chrono::steady_clock::now();
  for (int s = 1; s <= states; ++s) {
    std::vector<std::string> told = {individualIri(nextName), individualIri(nextName + 1)};
    nextName += 2;
    std::vector<std::string> retracted;
    if (s % 2 == 0 && !current.empty()) {
      retracted.push_back(current.front());
      current.pop_front();
    }
    std::vector<KoncludeClassAssertion> tell, retract;
    for (const std::string &i : told) {
      current.push_back(i);
    }
    for (const std::string &i : told) {
      tell.push_back({i.c_str(), kFather});
    }
    for (const std::string &i : retracted) {
      retract.push_back({i.c_str(), kFather});
    }
    // before the change: the worker is about to be replaced
    long rssBefore = client.workerRssKb();
    if (rssBefore > maxRss) {
      maxRss = rssBefore;
    }
    if (!client.tellAndRetract(tell, retract)) {
      ++failures;
    }
    for (int q = 0; q < 2; ++q) {
      if (!client.query(kQuery) ||
          client.resultRowCount() != static_cast<int>(current.size())) {
        ++failures;
      }
    }
    if (s % reportEvery == 0) {
      double blockMs = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - blockStart)
                           .count();
      totalMs += blockMs;
      long rss = client.workerRssKb();
      if (rss > maxRss) {
        maxRss = rss;
      }
      std::printf("%8d %8zu %14ld %14ld %12.2f %10d\n", s, current.size(), rss,
                  maxRss, blockMs / reportEvery, client.restartCount());
      std::fflush(stdout);
      blockStart = std::chrono::steady_clock::now();
    }
  }
  std::printf("total %.1f s, max worker RSS %ld kB, %d restarts, %d failure(s)\n",
              totalMs / 1000, maxRss, client.restartCount(), failures);
  return failures == 0 ? 0 : 1;
}
