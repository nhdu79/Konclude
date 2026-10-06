// konclude_worker: serves one embedded Konclude handle over a socket, so a
// host process (Powerlifted) can bound Konclude's memory by restarting the
// worker -- process exit frees everything, including leaks (see
// docs/INCREMENTAL_WORK_LOG.md, Task 5c, and README.md in this directory).
//
// Usage: konclude_worker <socket fd>
// Started by KoncludeWorkerClient (fork + exec); requests and responses are
// frames as in konclude_worker_protocol.h.

#include "konclude_embedded.h"
#include "konclude_worker_protocol.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace konclude_worker;

namespace {

Frame errorFrame(const std::string &message) {
  Frame frame{kError, {}};
  addItem(frame, message);
  return frame;
}

Frame lastErrorFrame(KoncludeReasonerHandle handle, const char *fallback) {
  const char *error = handle ? konclude_last_error(handle) : nullptr;
  return errorFrame(error && *error ? error : fallback);
}

Frame handleTellAndRetract(KoncludeReasonerHandle handle, const Frame &request) {
  if (request.items.size() < 4) {
    return errorFrame("malformed tell/retract request");
  }
  long counts[4];
  for (int i = 0; i < 4; ++i) {
    counts[i] = std::atol(request.items[i].value.c_str());
  }
  size_t expected = 4 + 2 * (counts[0] + counts[1]) + 3 * (counts[2] + counts[3]);
  if (counts[0] < 0 || counts[1] < 0 || counts[2] < 0 || counts[3] < 0 ||
      request.items.size() != expected) {
    return errorFrame("malformed tell/retract request");
  }
  // the structs point into request, which outlives the call
  std::vector<KoncludeClassAssertion> classLists[2];
  std::vector<KoncludeObjectPropertyAssertion> propertyLists[2];
  size_t next = 4;
  for (int list = 0; list < 2; ++list) {
    for (long i = 0; i < counts[list]; ++i, next += 2) {
      classLists[list].push_back({request.items[next].value.c_str(),
                                  request.items[next + 1].value.c_str()});
    }
  }
  for (int list = 0; list < 2; ++list) {
    for (long i = 0; i < counts[2 + list]; ++i, next += 3) {
      propertyLists[list].push_back({request.items[next].value.c_str(),
                                     request.items[next + 1].value.c_str(),
                                     request.items[next + 2].value.c_str()});
    }
  }
  if (!konclude_tell_and_retract_assertions(
          handle, classLists[0].data(), static_cast<int>(classLists[0].size()),
          classLists[1].data(), static_cast<int>(classLists[1].size()),
          propertyLists[0].data(), static_cast<int>(propertyLists[0].size()),
          propertyLists[1].data(), static_cast<int>(propertyLists[1].size()))) {
    return lastErrorFrame(handle, "tell/retract failed");
  }
  return Frame{kOk, {}};
}

Frame handleQuery(KoncludeReasonerHandle handle, const Frame &request) {
  if (request.items.size() != 1) {
    return errorFrame("malformed query request");
  }
  if (!konclude_execute_conjunctive_query(handle, request.items[0].value.c_str())) {
    return lastErrorFrame(handle, "query failed");
  }
  int variables = konclude_query_result_variable_count(handle);
  int rows = konclude_query_result_row_count(handle);
  Frame response{kOk, {}};
  addItem(response, std::to_string(variables));
  addItem(response, std::to_string(rows));
  for (int v = 0; v < variables; ++v) {
    addItem(response, konclude_query_result_variable_name(handle, v));
  }
  for (int r = 0; r < rows; ++r) {
    for (int v = 0; v < variables; ++v) {
      addItem(response, konclude_query_result_binding(handle, r, v));
    }
  }
  return response;
}

Frame handleCheck(KoncludeReasonerHandle handle, const Frame &request) {
  int result = 0;
  bool ok = false;
  if (request.kind == kCheckConsistency) {
    ok = konclude_check_consistency(handle, &result) != 0;
  } else if (request.items.size() == 1) {
    ok = konclude_check_satisfiability(handle, request.items[0].value.c_str(),
                                       &result) != 0;
  }
  if (!ok) {
    return lastErrorFrame(handle, "check failed");
  }
  Frame response{kOk, {}};
  addItem(response, result ? "1" : "0");
  return response;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "Usage: %s <socket fd> (started by KoncludeWorkerClient)\n",
                 argv[0]);
    return 2;
  }
  int fd = std::atoi(argv[1]);

  KoncludeReasonerHandle handle = nullptr;
  Frame request;
  while (readFrame(fd, &request)) {
    if (request.kind == kQuit) {
      break;
    }
    Frame response;
    if (request.kind == kLoadOntology) {
      if (handle) {
        response = errorFrame("the worker's ontology is already loaded");
      } else if (request.items.size() != 1) {
        response = errorFrame("malformed load request");
      } else {
        handle = konclude_create_reasoner();
        if (handle &&
            konclude_load_ontology_file(handle, request.items[0].value.c_str())) {
          response = Frame{kOk, {}};
        } else {
          response = lastErrorFrame(handle, "loading the ontology failed");
        }
      }
    } else if (!handle) {
      response = errorFrame("no ontology loaded");
    } else if (request.kind == kTellAndRetract) {
      response = handleTellAndRetract(handle, request);
    } else if (request.kind == kQuery) {
      response = handleQuery(handle, request);
    } else if (request.kind == kCheckConsistency ||
               request.kind == kCheckSatisfiable) {
      response = handleCheck(handle, request);
    } else {
      response = errorFrame("unknown request kind");
    }
    if (!writeFrame(fd, response)) {
      break;
    }
  }
  // Exiting frees all of Konclude's memory; tearing the handle down first
  // would only cost time.
  std::fflush(stdout);
  std::fflush(stderr);
  _exit(0);
}
