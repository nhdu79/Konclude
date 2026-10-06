// KoncludeWorkerClient: runs Konclude in a konclude_worker child process and
// restarts it every restartEvery state changes, re-telling the current state.
// Process exit frees all of the worker's memory, so memory is bounded by what
// one worker accumulates in restartEvery changes (see README.md here).
//
// Same methods as Powerlifted's KoncludeReasoner (src/search/konclude_reasoner.h),
// plus result accessors. Needs only konclude_embedded.h for the assertion
// structs; it does not link libKonclude or Qt. Not thread-safe; at most one
// state change or query at a time.

#ifndef KONCLUDE_WORKER_CLIENT_H
#define KONCLUDE_WORKER_CLIENT_H

#include "konclude_embedded.h"

#include <sys/types.h>

#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace konclude_worker {
struct Frame;
}

class KoncludeWorkerClient {
public:
  // workerPath: the konclude_worker executable. restartEvery: restart the
  // worker before a state change once this many changes ran on it (0: never;
  // a crashed worker is restarted either way).
  KoncludeWorkerClient(const std::string &workerPath, int restartEvery);
  ~KoncludeWorkerClient();
  KoncludeWorkerClient(const KoncludeWorkerClient &) = delete;
  KoncludeWorkerClient &operator=(const KoncludeWorkerClient &) = delete;

  // false if the worker could not be started (see lastError()).
  bool ok() const;

  bool loadOntology(const std::string &path);

  bool tellAndRetract(const std::vector<KoncludeClassAssertion> &tell,
                      const std::vector<KoncludeClassAssertion> &retract);
  bool tellAndRetractAssertions(
      const std::vector<KoncludeClassAssertion> &tellClass,
      const std::vector<KoncludeClassAssertion> &retractClass,
      const std::vector<KoncludeObjectPropertyAssertion> &tellProperty,
      const std::vector<KoncludeObjectPropertyAssertion> &retractProperty);

  // Results stay readable until the next query.
  bool query(const std::string &sparql);
  int resultRowCount() const;
  int resultVariableCount() const;
  const char *resultVariableName(int varIndex) const;
  const char *resultBinding(int row, int varIndex) const; // nullptr if unbound

  bool checkConsistency(bool *consistent);
  bool checkSatisfiability(const std::string &classIRI, bool *satisfiable);

  const char *lastError() const;

  // Diagnostics.
  pid_t workerPid() const;
  long workerRssKb() const; // Linux: VmRSS of the worker, -1 if unknown
  int restartCount() const;

private:
  typedef std::pair<std::string, std::string> ClassFact;
  typedef std::tuple<std::string, std::string, std::string> PropertyFact;

  bool startWorker();
  void stopWorker(bool graceful);
  // (Re)starts the worker, loads the ontology and tells the current state.
  bool restartWorker();
  // Sends a request and reads the response. If the worker died, restarts it
  // (which re-tells the tracked state) and retries once.
  bool roundTrip(const konclude_worker::Frame &request,
                 konclude_worker::Frame *response);
  bool sendTellAndRetract(const std::vector<ClassFact> &tellClass,
                          const std::vector<ClassFact> &retractClass,
                          const std::vector<PropertyFact> &tellProperty,
                          const std::vector<PropertyFact> &retractProperty);

  std::string mWorkerPath;
  int mRestartEvery;
  pid_t mPid;
  int mFd;
  std::string mOntologyPath;
  bool mOntologyLoaded;
  int mChangesOnWorker;
  int mRestarts;
  std::set<ClassFact> mClassFacts;
  std::set<PropertyFact> mPropertyFacts;
  std::vector<std::string> mVariableNames;
  std::vector<std::string> mBindings;
  std::vector<bool> mBindingNull;
  int mRows;
  std::string mLastError;
};

#endif // KONCLUDE_WORKER_CLIENT_H
