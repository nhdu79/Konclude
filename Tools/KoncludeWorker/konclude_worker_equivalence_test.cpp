// Equivalence test for KoncludeWorkerClient: runs one seeded random sequence
// of state changes both in-process (embedded C API) and through the worker,
// and compares the answers of three queries after every change. The worker
// restarts every restartEvery changes (re-telling the state) and is killed
// with SIGKILL twice to exercise crash recovery.
//
// TBox Tests/embedded-state-tbox-roles.owl.xml: Parent == hasChild some
// Person, domain(hasChild) = Person -- the answers depend on inference over
// both class and property facts.
//
// Build: Tools/KoncludeWorker/build.sh (from repo root)
// Run:   ./konclude_worker_equivalence_test [steps] [restartEvery]
// Defaults: 150 7. Exit code 0 = all answers equal.

#include "konclude_embedded.h"
#include "konclude_worker_client.h"

#include <signal.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace {

const char *kOntologyPath = "Tests/embedded-state-tbox-roles.owl.xml";
const char *kPerson = "http://example.org/test#Person";
const char *kHasChild = "http://example.org/test#hasChild";
const int kIndividuals = 12;

const char *kQueries[] = {
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX ex: <http://example.org/test#>\n"
    "SELECT ?x WHERE { ?x rdf:type ex:Parent . }",
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX ex: <http://example.org/test#>\n"
    "SELECT ?x WHERE { ?x rdf:type ex:Person . }",
    "PREFIX ex: <http://example.org/test#>\n"
    "SELECT ?x ?y WHERE { ?x ex:hasChild ?y . }"};

std::string individualIri(int idx) {
  return "http://example.org/test#p" + std::to_string(idx);
}

typedef std::vector<std::string> Row;

std::vector<Row> embeddedAnswer(KoncludeReasonerHandle handle, const char *query,
                                bool *ok) {
  std::vector<Row> rows;
  *ok = konclude_execute_conjunctive_query(handle, query) != 0;
  for (int r = 0; *ok && r < konclude_query_result_row_count(handle); ++r) {
    Row row;
    for (int v = 0; v < konclude_query_result_variable_count(handle); ++v) {
      const char *binding = konclude_query_result_binding(handle, r, v);
      row.push_back(binding ? binding : "<unbound>");
    }
    rows.push_back(row);
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

std::vector<Row> workerAnswer(KoncludeWorkerClient &client, const char *query,
                              bool *ok) {
  std::vector<Row> rows;
  *ok = client.query(query);
  for (int r = 0; *ok && r < client.resultRowCount(); ++r) {
    Row row;
    for (int v = 0; v < client.resultVariableCount(); ++v) {
      const char *binding = client.resultBinding(r, v);
      row.push_back(binding ? binding : "<unbound>");
    }
    rows.push_back(row);
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

} // namespace

int main(int argc, char **argv) {
  int steps = argc >= 2 ? std::atoi(argv[1]) : 150;
  int restartEvery = argc >= 3 ? std::atoi(argv[2]) : 7;

  KoncludeReasonerHandle handle = konclude_create_reasoner();
  if (!handle || !konclude_load_ontology_file(handle, kOntologyPath)) {
    std::fprintf(stderr, "in-process setup failed\n");
    return 2;
  }
  KoncludeWorkerClient client("./konclude_worker", restartEvery);
  if (!client.ok() || !client.loadOntology(kOntologyPath)) {
    std::fprintf(stderr, "worker setup failed: %s\n", client.lastError());
    return 2;
  }

  std::set<int> persons;
  std::set<std::pair<int, int>> children;
  unsigned int seed = 4711;
  auto nextRandom = [&seed](int bound) {
    seed = seed * 1103515245u + 12345u;
    return static_cast<int>((seed >> 8) % static_cast<unsigned int>(bound));
  };

  int mismatches = 0, failures = 0, kills = 0;
  for (int step = 1; step <= steps; ++step) {
    if (step == steps / 3 || step == 2 * steps / 3) {
      // a crashed worker must be restarted transparently
      ::kill(client.workerPid(), SIGKILL);
      ++kills;
    }
    // 1-3 random facts to toggle (tell if absent, retract if present)
    std::vector<std::string> strings; // keeps the IRIs alive for the structs
    strings.reserve(64);
    std::vector<KoncludeClassAssertion> tellC, retractC;
    std::vector<KoncludeObjectPropertyAssertion> tellP, retractP;
    std::set<int> touchedPersons;
    std::set<std::pair<int, int>> touchedChildren;
    for (int k = 0, n = 1 + nextRandom(3); k < n; ++k) {
      if (nextRandom(2) == 0) {
        int i = nextRandom(kIndividuals);
        if (!touchedPersons.insert(i).second) {
          continue;
        }
        strings.push_back(individualIri(i));
        KoncludeClassAssertion a{strings.back().c_str(), kPerson};
        if (persons.count(i)) {
          retractC.push_back(a);
          persons.erase(i);
        } else {
          tellC.push_back(a);
          persons.insert(i);
        }
      } else {
        std::pair<int, int> c(nextRandom(kIndividuals), nextRandom(kIndividuals));
        if (!touchedChildren.insert(c).second) {
          continue;
        }
        strings.push_back(individualIri(c.first));
        const char *s = strings.back().c_str();
        strings.push_back(individualIri(c.second));
        KoncludeObjectPropertyAssertion a{s, kHasChild, strings.back().c_str()};
        if (children.count(c)) {
          retractP.push_back(a);
          children.erase(c);
        } else {
          tellP.push_back(a);
          children.insert(c);
        }
      }
    }
    if (!konclude_tell_and_retract_assertions(
            handle, tellC.data(), static_cast<int>(tellC.size()), retractC.data(),
            static_cast<int>(retractC.size()), tellP.data(),
            static_cast<int>(tellP.size()), retractP.data(),
            static_cast<int>(retractP.size()))) {
      std::fprintf(stderr, "step %d: in-process tell/retract failed\n", step);
      ++failures;
    }
    if (!client.tellAndRetractAssertions(tellC, retractC, tellP, retractP)) {
      std::fprintf(stderr, "step %d: worker tell/retract failed: %s\n", step,
                   client.lastError());
      ++failures;
    }
    for (const char *query : kQueries) {
      bool embeddedOk = false, workerOk = false;
      std::vector<Row> expected = embeddedAnswer(handle, query, &embeddedOk);
      std::vector<Row> actual = workerAnswer(client, query, &workerOk);
      if (!embeddedOk || !workerOk) {
        std::fprintf(stderr, "step %d: query failed (in-process %d, worker %d: %s)\n",
                     step, embeddedOk, workerOk, client.lastError());
        ++failures;
      } else if (expected != actual) {
        std::fprintf(stderr, "step %d: answers differ (%zu in-process vs %zu worker rows)\n",
                     step, expected.size(), actual.size());
        ++mismatches;
      }
    }
  }
  std::printf("%d steps, %d restarts (%d after SIGKILL), %d mismatch(es), %d "
              "failure(s); final state %zu Person + %zu hasChild facts\n",
              steps, client.restartCount(), kills, mismatches, failures,
              persons.size(), children.size());
  konclude_destroy_reasoner(handle);
  return mismatches == 0 && failures == 0 ? 0 : 1;
}
