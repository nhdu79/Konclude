// Verification driver for ObjectPropertyAssertion support in
// konclude_tell_and_retract_assertions (see
// Source/Control/Interface/Embedded/konclude_embedded.h).
//
// TBox (Tests/embedded-state-tbox-roles.owl.xml):
//   ObjectPropertyDomain(hasChild, Person)
//   Parent == hasChild some Person
// So Parent answers depend on property *and* class facts together.
//
// Build: Tools/EmbeddedDriver/rebuild.sh (from repo root)
// Run:   ./embedded_property_driver [Tests/embedded-state-tbox-roles.owl.xml]

#include "konclude_embedded.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

const char *kJohn = "http://example.org/test#john";
const char *kMary = "http://example.org/test#mary";
const char *kBob = "http://example.org/test#bob";
const char *kPerson = "http://example.org/test#Person";
const char *kHasChild = "http://example.org/test#hasChild";

const char *kQueryParents =
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX ex: <http://example.org/test#>\n"
    "SELECT ?x WHERE { ?x rdf:type ex:Parent . }";

const char *kQueryPersons =
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX ex: <http://example.org/test#>\n"
    "SELECT ?x WHERE { ?x rdf:type ex:Person . }";

const char *kQueryHasChild =
    "PREFIX ex: <http://example.org/test#>\n"
    "SELECT ?x ?y WHERE { ?x ex:hasChild ?y . }";

typedef std::set<std::string> Answers;
typedef std::vector<KoncludeClassAssertion> ClassFacts;
typedef std::vector<KoncludeObjectPropertyAssertion> PropertyFacts;

int gFailures = 0;

// One answer per row; multi-column rows are joined with a space.
Answers runQuery(KoncludeReasonerHandle handle, const char *query) {
  Answers results;
  if (!konclude_execute_conjunctive_query(handle, query)) {
    std::fprintf(stderr, "  query failed: %s\n", konclude_last_error(handle));
    ++gFailures;
    return results;
  }
  int rowCount = konclude_query_result_row_count(handle);
  int varCount = konclude_query_result_variable_count(handle);
  for (int row = 0; row < rowCount; ++row) {
    std::string answer;
    for (int var = 0; var < varCount; ++var) {
      if (var > 0) {
        answer += " ";
      }
      std::string binding = konclude_query_result_binding(handle, row, var);
      answer += binding.substr(binding.find('#') + 1);
    }
    results.insert(answer);
  }
  return results;
}

bool transition(KoncludeReasonerHandle handle, const ClassFacts &tellClasses,
                const ClassFacts &retractClasses,
                const PropertyFacts &tellProperties,
                const PropertyFacts &retractProperties) {
  if (!konclude_tell_and_retract_assertions(
          handle, tellClasses.data(), static_cast<int>(tellClasses.size()),
          retractClasses.data(), static_cast<int>(retractClasses.size()),
          tellProperties.data(), static_cast<int>(tellProperties.size()),
          retractProperties.data(),
          static_cast<int>(retractProperties.size()))) {
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
    std::printf(" [%s]", a.c_str());
  }
  std::printf(" }\n");
  ++gFailures;
}

} // namespace

int main(int argc, char **argv) {
  std::string ontologyPath =
      argc >= 2 ? argv[1] : "Tests/embedded-state-tbox-roles.owl.xml";

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

  std::printf("--- State 1: tell hasChild(john, mary), Person(mary) ---\n");
  transition(handle, {{kMary, kPerson}}, {}, {{kJohn, kHasChild, kMary}}, {});
  expect(handle, kQueryHasChild, {"john mary"}, "hasChild = {(john, mary)}");
  expect(handle, kQueryParents, {"john"}, "Parent = {john} (via hasChild some Person)");
  expect(handle, kQueryPersons, {"john", "mary"},
         "Person = {john, mary} (john via the hasChild domain)");

  std::printf("--- State 2: retract hasChild(john, mary) ---\n");
  transition(handle, {}, {}, {}, {{kJohn, kHasChild, kMary}});
  expect(handle, kQueryHasChild, {}, "hasChild = {}");
  expect(handle, kQueryParents, {}, "Parent = {}");
  expect(handle, kQueryPersons, {"mary"}, "Person = {mary}");

  std::printf("--- State 3: re-tell hasChild(john, mary), tell hasChild(mary, "
              "bob), Person(bob) ---\n");
  transition(handle, {{kBob, kPerson}}, {},
             {{kJohn, kHasChild, kMary}, {kMary, kHasChild, kBob}}, {});
  expect(handle, kQueryHasChild, {"john mary", "mary bob"},
         "hasChild = {(john, mary), (mary, bob)}");
  expect(handle, kQueryParents, {"john", "mary"}, "Parent = {john, mary}");

  std::printf("--- State 4: retract Person(bob) (mixed with a no-op "
              "property retract) ---\n");
  transition(handle, {}, {{kBob, kPerson}}, {}, {{kBob, kHasChild, kJohn}});
  expect(handle, kQueryParents, {"john"},
         "Parent = {john} (mary's child bob is no longer a Person)");
  expect(handle, kQueryHasChild, {"john mary", "mary bob"},
         "hasChild unchanged");

  std::printf("--- Invalid call: null IRI must fail and change nothing ---\n");
  KoncludeObjectPropertyAssertion invalid = {kJohn, nullptr, kBob};
  if (konclude_tell_and_retract_assertions(handle, nullptr, 0, nullptr, 0,
                                           &invalid, 1, nullptr, 0)) {
    std::printf("  FAIL: call with a null IRI succeeded\n");
    ++gFailures;
  } else {
    std::printf("  OK: rejected (%s)\n", konclude_last_error(handle));
  }
  expect(handle, kQueryHasChild, {"john mary", "mary bob"},
         "hasChild unchanged after the rejected call");

  konclude_destroy_reasoner(handle);

  if (gFailures == 0) {
    std::printf("PASS: all checks passed.\n");
    return 0;
  }
  std::printf("FAIL: %d check(s) failed.\n", gFailures);
  return 1;
}
