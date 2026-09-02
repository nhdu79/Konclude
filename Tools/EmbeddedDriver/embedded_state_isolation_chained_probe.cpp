// Correctness probe for docs/EMBEDDED_STATE_ISOLATION_BUG.md, testing the
// experimental CEmbeddedChainedOntologyLoader/CEmbeddedChainedQueryManager
// pair (Source/Control/Interface/Embedded/CEmbeddedChained*.h/.cpp) via
// their own konclude_state_*_chained/konclude_execute_conjunctive_query_chained/
// konclude_chained_query_result_* C API functions (konclude_embedded.h) --
// deliberately separate classes/files/API functions from the production
// konclude_state_begin/konclude_state_assert_class_fact/
// konclude_execute_conjunctive_query/konclude_query_result_* path, so this
// experiment never touches or shares state with it.
//
// Unlike konclude_state_assert_class_fact (which accumulates every Tell
// into one shared, lazily-installed state revision -- the setup the
// isolation bug doc reproduces the bug against), the _chained variants
// create a brand-new revision layered on the current installed head, Tell/
// retract the single fact into it, and install it immediately -- mirroring
// COWLlinkProcessor.cpp's SPARQL_UPDATE_MODIFY handling call-for-call. This
// reproduces the EXACT repro from docs/EMBEDDED_STATE_ISOLATION_BUG.md's
// "Symptom" section, but through the chained API, to test that doc's
// question of whether the real OWLlink/SPARQL server Tell/Install pattern
// is itself subject to the same reused-individual-invisible bug -- see
// that doc's "Confirmed empirically" entry for the answer this probe
// found.
//
// Build (from repo root, after building KoncludeEmbedded.pro):
//   g++ -std=c++11 -I Source/Control/Interface/Embedded \
//       Tools/EmbeddedDriver/embedded_state_isolation_chained_probe.cpp \
//       -L ReleaseEmbedded -lKonclude -Wl,-rpath,ReleaseEmbedded \
//       -o embedded_state_isolation_chained_probe
// Run:
//   ./embedded_state_isolation_chained_probe [Tests/embedded-state-tbox-only.owl.xml]

#include "konclude_embedded.h"

#include <cstdio>
#include <set>
#include <string>

namespace {

const char *kA0 = "http://example.org/test#a0";
const char *kA1 = "http://example.org/test#a1";
const char *kA2 = "http://example.org/test#a2";
const char *kClassFather = "http://example.org/test#Father";

const char *kQueryFathers =
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX ex: <http://example.org/test#>\n"
    "SELECT ?x WHERE { ?x rdf:type ex:Father . }";

int gFailures = 0;

std::set<std::string> runQuery(KoncludeReasonerHandle handle,
                               const char *query) {
  std::set<std::string> results;
  if (!konclude_execute_conjunctive_query_chained(handle, query)) {
    std::fprintf(stderr, "  query failed: %s\n", konclude_last_error(handle));
    return results;
  }
  int rowCount = konclude_chained_query_result_row_count(handle);
  std::fprintf(stderr, "  [debug] rowCount=%d\n", rowCount);
  for (int row = 0; row < rowCount; ++row) {
    const char *binding = konclude_chained_query_result_binding(handle, row, 0);
    std::fprintf(stderr, "  [debug] row %d: %s\n", row, binding);
    results.insert(binding);
  }
  return results;
}

void expect(bool condition, const char *description) {
  if (condition) {
    std::printf("  OK: %s\n", description);
  } else {
    std::printf("  FAIL: %s\n", description);
    ++gFailures;
  }
  std::fflush(stdout);
}

bool assertChained(KoncludeReasonerHandle handle, const char *indiIRI,
                    const char *classIRI) {
  if (!konclude_state_assert_class_fact_chained(handle, indiIRI, classIRI)) {
    std::fprintf(stderr, "assert_class_fact_chained(%s, %s) failed: %s\n",
                 indiIRI, classIRI, konclude_last_error(handle));
    return false;
  }
  return true;
}

bool retractChained(KoncludeReasonerHandle handle, const char *indiIRI,
                     const char *classIRI) {
  if (!konclude_state_retract_class_fact_chained(handle, indiIRI, classIRI)) {
    std::fprintf(stderr, "retract_class_fact_chained(%s, %s) failed: %s\n",
                 indiIRI, classIRI, konclude_last_error(handle));
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  std::string ontologyPath =
      argc >= 2 ? argv[1] : "Tests/embedded-state-tbox-only.owl.xml";

  std::printf("Initializing embedded Konclude reasoner...\n");
  std::fflush(stdout);
  KoncludeReasonerHandle handle = konclude_create_reasoner();
  if (!handle) {
    std::fprintf(stderr, "konclude_create_reasoner failed\n");
    return 2;
  }

  std::printf("Loading TBox-only ontology: %s\n", ontologyPath.c_str());
  std::fflush(stdout);
  if (!konclude_load_ontology_file(handle, ontologyPath.c_str())) {
    std::fprintf(stderr, "load failed: %s\n", konclude_last_error(handle));
    konclude_destroy_reasoner(handle);
    return 2;
  }

  std::printf("\n--- State 1: chained-assert a0, query ---\n");
  std::fflush(stdout);
  if (!konclude_state_begin_chained(handle)) {
    std::fprintf(stderr, "state_begin_chained failed: %s\n", konclude_last_error(handle));
    konclude_destroy_reasoner(handle);
    return 2;
  }
  if (!assertChained(handle, kA0, kClassFather)) {
    konclude_destroy_reasoner(handle);
    return 2;
  }
  std::set<std::string> state1 = runQuery(handle, kQueryFathers);
  expect(state1.count(kA0) == 1 && state1.size() == 1,
         "state 1: query contains exactly a0 (this also installs state 1's "
         "chain head)");

  std::printf("\n--- State 2: re-tell a0 (reused name) + tell a1 (new "
              "name), both chained, query ---\n");
  std::fflush(stdout);
  if (!konclude_state_begin_chained(handle)) {
    std::fprintf(stderr, "state_begin_chained failed: %s\n", konclude_last_error(handle));
    konclude_destroy_reasoner(handle);
    return 2;
  }
  if (!assertChained(handle, kA0, kClassFather) ||
      !assertChained(handle, kA1, kClassFather)) {
    konclude_destroy_reasoner(handle);
    return 2;
  }
  std::set<std::string> state2 = runQuery(handle, kQueryFathers);
  expect(state2.count(kA1) == 1,
         "state 2: query contains a1 (brand-new individual name)");
  expect(state2.count(kA0) == 1,
         "state 2: query contains a0 (REUSED individual name -- this is "
         "exactly what docs/EMBEDDED_STATE_ISOLATION_BUG.md's non-chained "
         "repro drops)");
  expect(state2.size() == 2, "state 2: query contains exactly {a0, a1}, "
                             "nothing extra");

  std::printf("\n--- State 3: chained-retract a1, query (should drop back "
              "to {a0}) ---\n");
  std::fflush(stdout);
  if (!retractChained(handle, kA1, kClassFather)) {
    konclude_destroy_reasoner(handle);
    return 2;
  }
  std::set<std::string> state2AfterRetract = runQuery(handle, kQueryFathers);
  expect(state2AfterRetract.count(kA1) == 0,
         "after chained retract: a1 no longer a Father");
  expect(state2AfterRetract.count(kA0) == 1,
         "after chained retract: a0 (unrelated, reused-name individual) "
         "still a Father");

  std::printf("\n--- State 4: fresh state, a0/a1 must not leak forward, "
              "tell a0+a1+a2 chained, query ---\n");
  std::fflush(stdout);
  if (!konclude_state_begin_chained(handle)) {
    std::fprintf(stderr, "state_begin_chained failed: %s\n", konclude_last_error(handle));
    konclude_destroy_reasoner(handle);
    return 2;
  }
  std::set<std::string> state4Empty = runQuery(handle, kQueryFathers);
  expect(state4Empty.empty(),
         "fresh state 4: query is empty before any assert (no leak-forward "
         "-- see docs/EMBEDDED_STATE_ISOLATION_BUG.md for why this can fail "
         "with the chained API even when state isolation itself is fine: "
         "every install advances the KB's single 'current' revision, so a "
         "fresh chained state layers on whatever the LAST chained state "
         "left installed, not on the original loaded ontology)");
  if (!assertChained(handle, kA0, kClassFather) ||
      !assertChained(handle, kA1, kClassFather) ||
      !assertChained(handle, kA2, kClassFather)) {
    konclude_destroy_reasoner(handle);
    return 2;
  }
  std::set<std::string> state4 = runQuery(handle, kQueryFathers);
  expect(state4.count(kA0) == 1 && state4.count(kA1) == 1 &&
             state4.count(kA2) == 1 && state4.size() == 3,
         "state 4: query contains exactly {a0, a1, a2} -- generalizes to a "
         "3rd state, matching the isolation bug doc's own generalization "
         "check");

  konclude_destroy_reasoner(handle);

  if (gFailures == 0) {
    std::printf("\nPASS: all checks passed -- the chained Tell/Install "
                "pattern does NOT reproduce the isolation bug.\n");
    return 0;
  } else {
    std::printf("\nFAIL: %d check(s) failed -- the chained Tell/Install "
                "pattern DOES reproduce the isolation bug (or something "
                "else broke).\n", gFailures);
    return 1;
  }
}
