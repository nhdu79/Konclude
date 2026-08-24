// Standalone smoke test for the embedded C API's conjunctive-query support
// (konclude_execute_conjunctive_query and friends, see
// Source/Control/Interface/Embedded/konclude_embedded.h).
//
// Loads an ontology through the embedded interface, runs the same query as
// Tests/roberts-family-simple-cq-test.sparql, and diffs the (x,y) pairs
// against the trusted CLI-computed answer set in cq-answers.xml (produced
// via `./Konclude sparqlfile -s Tests/roberts-family-simple-cq-test.sparql
// -i Tests/roberts-family-full-D.owl.xml -o cq-answers.xml`).
//
// Build (from repo root, after building KoncludeEmbedded.pro):
//   g++ -std=c++11 -I Source/Control/Interface/Embedded \
//       Tools/EmbeddedDriver/embedded_cq_driver.cpp \
//       -L ReleaseEmbedded -lKonclude -Wl,-rpath,ReleaseEmbedded \
//       -o embedded_cq_driver
// Run (arguments optional -- defaults to the same fixture pair, useful when
// launching from a debugger with no `args` configured):
//   ./embedded_cq_driver [Tests/roberts-family-full-D.owl.xml cq-answers.xml]

#include "konclude_embedded.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <utility>

namespace {

const char *kQuery =
    "PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>\n"
    "PREFIX fam: <http://www.co-ode.org/roberts/family-tree.owl#>\n"
    "SELECT ?x ?y WHERE { ?x rdf:type fam:Father . ?x fam:hasChild ?y . }";

typedef std::pair<std::string, std::string> StringPair;

// Extremely small, purpose-built extraction of (x,y) <uri> pairs from the
// SPARQL-XML answers file produced by `./Konclude sparqlfile` -- not a
// general XML parser, just enough structure since we control the exact
// output format on both sides (one <binding name="x"|"y"><uri>...</uri>
// per line, x immediately followed by y within a <result>).
std::set<StringPair> loadExpectedAnswers(const std::string &path) {
  std::set<StringPair> expected;
  std::ifstream in(path.c_str());
  if (!in) {
    std::cerr << "Could not open expected-answers file: " << path << std::endl;
    return expected;
  }
  std::string line;
  std::string pendingX;
  bool haveX = false;
  while (std::getline(in, line)) {
    size_t uriStart = line.find("<uri>");
    size_t uriEnd = line.find("</uri>");
    if (uriStart == std::string::npos || uriEnd == std::string::npos ||
        uriEnd <= uriStart) {
      continue;
    }
    std::string uri = line.substr(uriStart + 5, uriEnd - (uriStart + 5));
    if (line.find("name=\"x\"") != std::string::npos) {
      pendingX = uri;
      haveX = true;
    } else if (line.find("name=\"y\"") != std::string::npos && haveX) {
      expected.insert(std::make_pair(pendingX, uri));
      haveX = false;
    }
  }
  return expected;
}

} // namespace

int main(int argc, char **argv) {
  // Defaults mirror the exact fixture pair documented in
  // docs/EMBEDDED_CQ_DRIVER.md, so the driver still runs with no
  // command-line arguments at all -- e.g. under a debugger where
  // configuring `args` is an easy step to forget. Pass two arguments to
  // override.
  std::string ontologyPath = "Tests/roberts-family-full-D.owl.xml";
  std::string expectedAnswersPath = "cq-answers.xml";
  if (argc >= 3) {
    ontologyPath = argv[1];
    expectedAnswersPath = argv[2];
  } else if (argc != 1) {
    std::cerr << "Usage: " << argv[0]
              << " [<ontology.owl.xml> <expected-answers.xml>]" << std::endl;
    return 2;
  }

  std::cout << "Initializing embedded Konclude reasoner..." << std::endl;
  KoncludeReasonerHandle handle = konclude_create_reasoner();
  if (!handle) {
    std::cerr << "konclude_create_reasoner failed" << std::endl;
    return 1;
  }

  std::cout << "Loading ontology..." << std::endl;
  if (!konclude_load_ontology_file(handle, ontologyPath.c_str())) {
    std::cerr << "konclude_load_ontology_file failed: "
              << konclude_last_error(handle) << std::endl;
    konclude_destroy_reasoner(handle);
    return 1;
  }

  if (!konclude_state_begin(handle)) {
    std::cerr << "konclude_state_begin failed: "
              << konclude_last_error(handle) << std::endl;
    konclude_destroy_reasoner(handle);
    return 1;
  }

  if (!konclude_execute_conjunctive_query(handle, kQuery)) {
    std::cerr << "konclude_execute_conjunctive_query failed: "
              << konclude_last_error(handle) << std::endl;
    konclude_destroy_reasoner(handle);
    return 1;
  }

  int rowCount = konclude_query_result_row_count(handle);
  int varCount = konclude_query_result_variable_count(handle);
  std::cout << "Got " << rowCount << " rows, " << varCount << " variables";
  if (varCount == 2) {
    std::cout << " (" << konclude_query_result_variable_name(handle, 0) << ", "
              << konclude_query_result_variable_name(handle, 1) << ")";
  }
  std::cout << "." << std::endl;

  if (varCount != 2) {
    std::cerr << "Expected 2 projected variables (x, y), got " << varCount
              << std::endl;
    konclude_destroy_reasoner(handle);
    return 1;
  }

  std::set<StringPair> actual;
  for (int row = 0; row < rowCount; ++row) {
    std::string x = konclude_query_result_binding(handle, row, 0);
    std::string y = konclude_query_result_binding(handle, row, 1);
    actual.insert(std::make_pair(x, y));
  }

  std::set<StringPair> expected = loadExpectedAnswers(expectedAnswersPath);
  std::cout << "Embedded interface: " << actual.size()
            << " unique (x,y) pairs. "
            << "Trusted CLI answer file: " << expected.size()
            << " unique pairs." << std::endl;

  bool ok = (actual == expected) && !expected.empty();
  if (!ok) {
    int shown = 0;
    for (std::set<StringPair>::const_iterator it = actual.begin();
         it != actual.end(); ++it) {
      if (expected.find(*it) == expected.end() && shown < 10) {
        std::cerr << "  UNEXPECTED (embedded only): " << it->first << " , "
                  << it->second << std::endl;
        ++shown;
      }
    }
    shown = 0;
    for (std::set<StringPair>::const_iterator it = expected.begin();
         it != expected.end(); ++it) {
      if (actual.find(*it) == actual.end() && shown < 10) {
        std::cerr << "  MISSING (expected only): " << it->first << " , "
                  << it->second << std::endl;
        ++shown;
      }
    }
  }

  konclude_destroy_reasoner(handle);

  if (ok) {
    std::cout << "PASS: embedded conjunctive-query results match the trusted "
                 "CLI (sparqlfile) output exactly."
              << std::endl;
    return 0;
  } else {
    std::cout << "FAIL: embedded conjunctive-query results differ from the "
                 "trusted CLI output."
              << std::endl;
    return 1;
  }
}
