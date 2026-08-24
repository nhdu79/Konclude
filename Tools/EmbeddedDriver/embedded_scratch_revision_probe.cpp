// Correctness probe for docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md's
// Decision 4 open item: does repeatedly building a scratch knowledge-base
// revision via CCreateKnowledgeBaseRevisionUpdateCommand -- WITHOUT ever
// installing it -- hang under CEmbeddedReasoner's current dispatch wiring?
// docs/FASTDOWNWARD_EMBEDDING.md records this exact command sequence
// hanging in an earlier pass of this project; this probe checks whether
// this session's CEmbeddedOWLlinkProcessor fix (which resolved a related
// hang in loadOntologyFile/checkConsistency/checkSatisfiability) also
// resolved it here, or whether it's a separate, still-open bug.
//
// This is a correctness gate, not a timing benchmark -- no stats
// collection, just "does every iteration return." Konclude's
// CCommandExecutedBlocker::waitExecutedCommand blocks indefinitely on a
// hang, so there is no in-process way to detect one; run this under a
// shell-level timeout instead. If it's killed, CEmbeddedReasoner's
// per-iteration stderr progress lines show exactly how far it got.
//
// Build (from repo root, after building KoncludeEmbedded.pro):
//   g++ -std=c++11 -O2 -I Source/Control/Interface/Embedded \
//       Tools/EmbeddedDriver/embedded_scratch_revision_probe.cpp \
//       -L ReleaseEmbedded -lKonclude -Wl,-rpath,ReleaseEmbedded \
//       -o embedded_scratch_revision_probe
// Run (kill it after 30s if it hasn't finished -- that's the hang signal):
//   timeout 30 ./embedded_scratch_revision_probe [ontologyPath] [iterations]

#include "konclude_embedded.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char **argv) {
  std::string ontologyPath = argc >= 2 ? argv[1] : "Tests/roberts-family-full-D.owl.xml";
  int iterations = argc >= 3 ? std::atoi(argv[2]) : 10;

  std::printf("Creating reasoner...\n");
  KoncludeReasonerHandle handle = konclude_create_reasoner();
  if (!handle) {
    std::fprintf(stderr, "konclude_create_reasoner failed\n");
    return 2;
  }

  std::printf("Loading ontology: %s\n", ontologyPath.c_str());
  if (!konclude_load_ontology_file(handle, ontologyPath.c_str())) {
    std::fprintf(stderr, "load failed: %s\n", konclude_last_error(handle));
    konclude_destroy_reasoner(handle);
    return 2;
  }

  std::printf("Probing %d never-install scratch-revision cycles...\n", iterations);
  int completed = konclude_probe_scratch_revision_cycles(handle, iterations);

  bool ok = completed == iterations;
  if (!ok) {
    std::fprintf(stderr, "FAIL: only %d/%d cycles completed: %s\n",
                 completed, iterations, konclude_last_error(handle));
  } else {
    std::printf("PASS: all %d cycles completed.\n", completed);
  }

  konclude_destroy_reasoner(handle);
  return ok ? 0 : 1;
}
