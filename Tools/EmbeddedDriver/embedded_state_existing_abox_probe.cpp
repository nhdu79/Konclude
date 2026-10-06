// One-off debugging probe: does konclude_tell_and_retract_axioms work when
// the BASE ontology already has ABox individuals/indices built (unlike
// embedded_state_driver.cpp's TBox-only fixture, which has zero
// individuals at all)? Isolates whether the "new fact invisible to CQ"
// problem is specific to an entirely-empty-ABox base ontology or general.
#include "konclude_embedded.h"
#include <cstdio>
#include <string>

int main() {
  KoncludeReasonerHandle handle = konclude_create_reasoner();
  if (!konclude_load_ontology_file(handle,
                                   "Tests/roberts-family-full-D.owl.xml")) {
    std::fprintf(stderr, "load failed: %s\n", konclude_last_error(handle));
    return 1;
  }
  const char *newIndi =
      "http://www.co-ode.org/roberts/family-tree.owl#ZZZNewFather";
  const char *fatherClass =
      "http://www.co-ode.org/roberts/family-tree.owl#Father";
  KoncludeClassAssertion fact = {newIndi, fatherClass};
  if (!konclude_tell_and_retract_axioms(handle, &fact, 1, nullptr, 0)) {
    std::fprintf(stderr, "assert failed: %s\n", konclude_last_error(handle));
    return 1;
  }
  const char *query =
      "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
      "PREFIX fam: <http://www.co-ode.org/roberts/family-tree.owl#>\n"
      "SELECT ?x WHERE { ?x rdf:type fam:Father . }";
  if (!konclude_execute_conjunctive_query(handle, query)) {
    std::fprintf(stderr, "query failed: %s\n", konclude_last_error(handle));
    return 1;
  }
  int rowCount = konclude_query_result_row_count(handle);
  std::printf("rowCount=%d\n", rowCount);
  bool found = false;
  for (int i = 0; i < rowCount; ++i) {
    const char *b = konclude_query_result_binding(handle, i, 0);
    std::string s(b);
    if (s == newIndi)
      found = true;
  }
  std::printf(found ? "FOUND new individual among Fathers\n"
                    : "new individual NOT found among Fathers\n");
  konclude_destroy_reasoner(handle);
  return 0;
}
