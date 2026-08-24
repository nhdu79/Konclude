// Verification driver for the new FD-facing "state" API
// (konclude_state_begin / konclude_state_assert_class_fact, see
// Source/Control/Interface/Embedded/konclude_embedded.h and
// docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md's Decision 2) -- exercises
// exactly the intended FD flow: load a TBox-only ontology once, then for
// each "state" call konclude_state_begin, assert some ClassAssertion ABox
// facts, and run potentially several CQs against that same state before
// moving to the next one.
//
// Checks three things a naive implementation could get wrong:
//   1. An asserted fact is visible to a query (and to reasoning over it --
//      the query asks for a superclass the fact doesn't literally have,
//      so this also proves classification runs over the asserted ABox,
//      not just literal triple matching).
//   2. Two queries against the SAME state both see the same asserted fact
//      (this is the whole point of decoupling revision creation from query
//      execution -- previously executeConjunctiveQuery() minted its own
//      throwaway revision per call, so asserted facts couldn't survive to
//      a second query).
//   3. Starting a NEW state (a second konclude_state_begin call) discards
//      the previous state's facts -- they must not leak forward.
//
// Build (from repo root, after building KoncludeEmbedded.pro):
//   g++ -std=c++11 -I Source/Control/Interface/Embedded \
//       Tools/EmbeddedDriver/embedded_state_driver.cpp \
//       -L ReleaseEmbedded -lKonclude -Wl,-rpath,ReleaseEmbedded \
//       -o embedded_state_driver
// Run:
//   ./embedded_state_driver [Tests/embedded-state-tbox-only.owl.xml]

#include "konclude_embedded.h"

#include <cstdio>
#include <set>
#include <string>

namespace {

const char *kIndividualJohn = "http://example.org/test#john";
const char *kIndividualMary = "http://example.org/test#mary";
const char *kClassFather = "http://example.org/test#Father";
const char *kClassPerson = "http://example.org/test#Person";

const char *kQueryPersons =
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX ex: <http://example.org/test#>\n"
    "SELECT ?x WHERE { ?x rdf:type ex:Person . }";

const char *kQueryFathers =
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX ex: <http://example.org/test#>\n"
    "SELECT ?x WHERE { ?x rdf:type ex:Father . }";

int gFailures = 0;

std::set<std::string> runQuery(KoncludeReasonerHandle handle,
                               const char *query) {
  std::set<std::string> results;
  if (!konclude_execute_conjunctive_query(handle, query)) {
    std::fprintf(stderr, "  query failed: %s\n", konclude_last_error(handle));
    return results;
  }
  int rowCount = konclude_query_result_row_count(handle);
  std::fprintf(stderr, "  [debug] rowCount=%d\n", rowCount);
  for (int row = 0; row < rowCount; ++row) {
    const char *binding = konclude_query_result_binding(handle, row, 0);
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
}

} // namespace

int main(int argc, char **argv) {
  std::string ontologyPath =
      argc >= 2 ? argv[1] : "Tests/embedded-state-tbox-only.owl.xml";

  std::printf("Initializing embedded Konclude reasoner...\n");
  KoncludeReasonerHandle handle = konclude_create_reasoner();
  if (!handle) {
    std::fprintf(stderr, "konclude_create_reasoner failed\n");
    return 2;
  }

  std::printf("Loading TBox-only ontology: %s\n", ontologyPath.c_str());
  if (!konclude_load_ontology_file(handle, ontologyPath.c_str())) {
    std::fprintf(stderr, "load failed: %s\n", konclude_last_error(handle));
    konclude_destroy_reasoner(handle);
    return 2;
  }

  std::printf("\n--- State 1: assert john is a Father, query twice ---\n");
  if (!konclude_state_begin(handle)) {
    std::fprintf(stderr, "state_begin failed: %s\n",
                 konclude_last_error(handle));
    konclude_destroy_reasoner(handle);
    return 2;
  }
  if (!konclude_state_assert_class_fact(handle, kIndividualJohn,
                                        kClassFather)) {
    std::fprintf(stderr, "assert_class_fact failed: %s\n",
                 konclude_last_error(handle));
    konclude_destroy_reasoner(handle);
    return 2;
  }

  std::set<std::string> fathers1 = runQuery(handle, kQueryFathers);
  expect(fathers1.count(kIndividualJohn) == 1,
         "first Father query (this state) contains john");

  std::set<std::string> persons1 = runQuery(handle, kQueryPersons);
  expect(persons1.count(kIndividualJohn) == 1,
         "Person query (this state) contains john via SubClassOf(Father, "
         "Person) -- proves reasoning ran over the asserted fact");

  std::set<std::string> fathers1Again = runQuery(handle, kQueryFathers);
  expect(fathers1Again.count(kIndividualJohn) == 1,
         "second Father query against the SAME state still contains john "
         "(same state reused across queries, not re-minted per call)");

  std::printf("\n--- State 2: fresh state, assert mary, john must not leak "
              "forward ---\n");
  if (!konclude_state_begin(handle)) {
    std::fprintf(stderr, "state_begin failed: %s\n",
                 konclude_last_error(handle));
    konclude_destroy_reasoner(handle);
    return 2;
  }
  if (!konclude_state_assert_class_fact(handle, kIndividualMary,
                                        kClassPerson)) {
    std::fprintf(stderr, "assert_class_fact failed: %s\n",
                 konclude_last_error(handle));
    konclude_destroy_reasoner(handle);
    return 2;
  }

  std::set<std::string> fathers2 = runQuery(handle, kQueryFathers);
  // expect fathers2 to be empty
  expect(fathers2.empty(),
         "new state's Father query is empty -- previous state's fact did not "
         "leak forward");

  std::set<std::string> persons2 = runQuery(handle, kQueryPersons);
  expect(persons2.count(kIndividualMary) == 1,
         "new state's Person query contains mary");

  konclude_destroy_reasoner(handle);

  if (gFailures == 0) {
    std::printf("\nPASS: all checks passed.\n");
    return 0;
  } else {
    std::printf("\nFAIL: %d check(s) failed.\n", gFailures);
    return 1;
  }
}
