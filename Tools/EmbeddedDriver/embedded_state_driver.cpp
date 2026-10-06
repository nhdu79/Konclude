// Verification driver for the FD/Powerlifted-facing state API
// (konclude_tell_and_retract_axioms + konclude_execute_conjunctive_query,
// see Source/Control/Interface/Embedded/konclude_embedded.h): load a
// TBox-only ontology once, then move through several states by passing
// only the tell/retract diff between consecutive states, querying each.
//
// Checks:
//   1. A told fact is visible, and reasoned over (Person via
//      SubClassOf(Father, Person)), and stays visible for a second query
//      against the same state.
//   2. A retracted fact disappears; an unchanged fact survives a diff.
//   3. Re-telling an individual that was already used in an earlier
//      (queried) state works -- the case docs/INCREMENTAL_REASONING.md §3
//      describes.
//
// Build: Tools/EmbeddedDriver/rebuild.sh (from repo root)
// Run:   ./embedded_state_driver [Tests/embedded-state-tbox-only.owl.xml]

#include "konclude_embedded.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

const char *kJohn = "http://example.org/test#john";
const char *kMary = "http://example.org/test#mary";
const char *kFather = "http://example.org/test#Father";
const char *kPerson = "http://example.org/test#Person";

const char *kQueryPersons =
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX ex: <http://example.org/test#>\n"
    "SELECT ?x WHERE { ?x rdf:type ex:Person . }";

const char *kQueryFathers =
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX ex: <http://example.org/test#>\n"
    "SELECT ?x WHERE { ?x rdf:type ex:Father . }";

typedef std::set<std::string> Answers;

int gFailures = 0;

Answers runQuery(KoncludeReasonerHandle handle, const char *query) {
  Answers results;
  if (!konclude_execute_conjunctive_query(handle, query)) {
    std::fprintf(stderr, "  query failed: %s\n", konclude_last_error(handle));
    ++gFailures;
    return results;
  }
  int rowCount = konclude_query_result_row_count(handle);
  for (int row = 0; row < rowCount; ++row) {
    results.insert(konclude_query_result_binding(handle, row, 0));
  }
  return results;
}

bool transition(KoncludeReasonerHandle handle,
                const std::vector<KoncludeClassAssertion> &tell,
                const std::vector<KoncludeClassAssertion> &retract) {
  if (!konclude_tell_and_retract_axioms(handle, tell.data(),
                                        static_cast<int>(tell.size()),
                                        retract.data(),
                                        static_cast<int>(retract.size()))) {
    std::fprintf(stderr, "  tell/retract failed: %s\n",
                 konclude_last_error(handle));
    ++gFailures;
    return false;
  }
  return true;
}

void expect(KoncludeReasonerHandle handle, const char *query,
            const Answers &expected, const char *description) {
  Answers actual = runQuery(handle, query);
  if (actual == expected) {
    std::printf("  OK: %s\n", description);
    return;
  }
  std::printf("  FAIL: %s -- got {", description);
  for (const std::string &a : actual) {
    std::printf(" %s", a.c_str());
  }
  std::printf(" }\n");
  ++gFailures;
}

} // namespace

int main(int argc, char **argv) {
  std::string ontologyPath =
      argc >= 2 ? argv[1] : "Tests/embedded-state-tbox-only.owl.xml";

  KoncludeReasonerHandle handle = konclude_create_reasoner();
  if (!handle) {
    std::fprintf(stderr, "konclude_create_reasoner failed\n");
    return 2;
  }
  if (!konclude_load_ontology_file(handle, ontologyPath.c_str())) {
    std::fprintf(stderr, "load failed: %s\n", konclude_last_error(handle));
    konclude_destroy_reasoner(handle);
    return 2;
  }

  std::printf("--- State 1: tell Father(john) ---\n");
  transition(handle, {{kJohn, kFather}}, {});
  expect(handle, kQueryFathers, {kJohn}, "Father = {john}");
  expect(handle, kQueryPersons, {kJohn}, "Person = {john} (via TBox)");
  expect(handle, kQueryFathers, {kJohn}, "Father = {john} again, same state");

  std::printf("--- State 2: retract Father(john), tell Person(mary) ---\n");
  transition(handle, {{kMary, kPerson}}, {{kJohn, kFather}});
  expect(handle, kQueryFathers, {}, "Father = {} (john retracted)");
  expect(handle, kQueryPersons, {kMary}, "Person = {mary}");

  std::printf("--- State 3: re-tell Father(john) (reused individual) ---\n");
  transition(handle, {{kJohn, kFather}}, {});
  expect(handle, kQueryFathers, {kJohn}, "Father = {john}");
  expect(handle, kQueryPersons, {kJohn, kMary},
         "Person = {john, mary} (mary unchanged)");

  std::printf("--- State 4: empty diff ---\n");
  transition(handle, {}, {});
  expect(handle, kQueryPersons, {kJohn, kMary}, "Person = {john, mary}");

  konclude_destroy_reasoner(handle);

  if (gFailures == 0) {
    std::printf("PASS: all checks passed.\n");
    return 0;
  }
  std::printf("FAIL: %d check(s) failed.\n", gFailures);
  return 1;
}
