// See konclude_worker_client.h and README.md in this directory.

#include "konclude_worker_client.h"
#include "konclude_worker_protocol.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using konclude_worker::Frame;
using konclude_worker::addItem;
using konclude_worker::readFrame;
using konclude_worker::writeFrame;

namespace {

// The fd number the worker gets its socket on.
const int kWorkerFd = 3;

} // namespace

KoncludeWorkerClient::KoncludeWorkerClient(const std::string &workerPath,
                                           int restartEvery)
    : mWorkerPath(workerPath), mRestartEvery(restartEvery), mPid(-1), mFd(-1),
      mOntologyLoaded(false), mChangesOnWorker(0), mRestarts(0), mRows(0) {
  startWorker();
}

KoncludeWorkerClient::~KoncludeWorkerClient() { stopWorker(true); }

bool KoncludeWorkerClient::ok() const { return mPid > 0 && mFd >= 0; }

bool KoncludeWorkerClient::startWorker() {
  int fds[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
    mLastError = std::string("socketpair failed: ") + std::strerror(errno);
    return false;
  }
  // prepared before fork: the child may only make async-signal-safe calls
  std::string fdArg = std::to_string(kWorkerFd);
  char *argv[] = {const_cast<char *>(mWorkerPath.c_str()),
                  const_cast<char *>(fdArg.c_str()), nullptr};
  pid_t pid = ::fork();
  if (pid < 0) {
    mLastError = std::string("fork failed: ") + std::strerror(errno);
    ::close(fds[0]);
    ::close(fds[1]);
    return false;
  }
  if (pid == 0) {
    // child: the socket at kWorkerFd, without close-on-exec; everything else
    // marked close-on-exec stays behind
    if (fds[1] == kWorkerFd) {
      ::fcntl(kWorkerFd, F_SETFD, 0);
    } else if (::dup2(fds[1], kWorkerFd) < 0) {
      _exit(127);
    }
    ::execv(argv[0], argv);
    _exit(127);
  }
  ::close(fds[1]);
  mPid = pid;
  mFd = fds[0];
  mChangesOnWorker = 0;
  return true;
}

void KoncludeWorkerClient::stopWorker(bool graceful) {
  if (mFd >= 0) {
    if (graceful) {
      writeFrame(mFd, Frame{konclude_worker::kQuit, {}});
    }
    ::close(mFd);
    mFd = -1;
  }
  if (mPid > 0) {
    if (!graceful) {
      // no teardown needed: process exit frees the worker's memory
      ::kill(mPid, SIGKILL);
    }
    int status = 0;
    while (::waitpid(mPid, &status, 0) < 0 && errno == EINTR) {
    }
    mPid = -1;
  }
}

namespace {

// One request/response without retry; false on an I/O error (worker gone).
bool exchange(int fd, const Frame &request, Frame *response) {
  return fd >= 0 && writeFrame(fd, request) && readFrame(fd, response);
}

void appendClassFacts(Frame &frame, const std::set<std::pair<std::string, std::string>> &facts) {
  for (const auto &fact : facts) {
    addItem(frame, fact.first);
    addItem(frame, fact.second);
  }
}

} // namespace

bool KoncludeWorkerClient::restartWorker() {
  stopWorker(false);
  ++mRestarts;
  if (!startWorker()) {
    return false;
  }
  if (!mOntologyLoaded) {
    return true;
  }
  Frame load{konclude_worker::kLoadOntology, {}};
  addItem(load, mOntologyPath);
  Frame response;
  if (!exchange(mFd, load, &response) || response.kind != konclude_worker::kOk) {
    mLastError = "restarting the worker: loading the ontology failed";
    return false;
  }
  if (mClassFacts.empty() && mPropertyFacts.empty()) {
    return true;
  }
  // the whole current state in one change
  Frame tell{konclude_worker::kTellAndRetract, {}};
  addItem(tell, std::to_string(mClassFacts.size()));
  addItem(tell, "0");
  addItem(tell, std::to_string(mPropertyFacts.size()));
  addItem(tell, "0");
  appendClassFacts(tell, mClassFacts);
  for (const PropertyFact &fact : mPropertyFacts) {
    addItem(tell, std::get<0>(fact));
    addItem(tell, std::get<1>(fact));
    addItem(tell, std::get<2>(fact));
  }
  if (!exchange(mFd, tell, &response) || response.kind != konclude_worker::kOk) {
    mLastError = "restarting the worker: re-telling the state failed";
    return false;
  }
  return true;
}

bool KoncludeWorkerClient::roundTrip(const Frame &request, Frame *response) {
  mLastError.clear();
  if (exchange(mFd, request, response)) {
    return true;
  }
  // the worker died (or never started): restart, re-tell the state, retry once
  if (!restartWorker() || !exchange(mFd, request, response)) {
    if (mLastError.empty()) {
      mLastError = "the Konclude worker is not responding";
    }
    return false;
  }
  return true;
}

bool KoncludeWorkerClient::loadOntology(const std::string &path) {
  if (mOntologyLoaded) {
    mLastError = "the ontology is already loaded";
    return false;
  }
  Frame request{konclude_worker::kLoadOntology, {}};
  addItem(request, path);
  Frame response;
  if (!roundTrip(request, &response)) {
    return false;
  }
  if (response.kind != konclude_worker::kOk) {
    mLastError = response.items.empty() ? "loading failed" : response.items[0].value;
    return false;
  }
  mOntologyPath = path;
  mOntologyLoaded = true;
  return true;
}

bool KoncludeWorkerClient::tellAndRetract(
    const std::vector<KoncludeClassAssertion> &tell,
    const std::vector<KoncludeClassAssertion> &retract) {
  return tellAndRetractAssertions(tell, retract, {}, {});
}

bool KoncludeWorkerClient::tellAndRetractAssertions(
    const std::vector<KoncludeClassAssertion> &tellClass,
    const std::vector<KoncludeClassAssertion> &retractClass,
    const std::vector<KoncludeObjectPropertyAssertion> &tellProperty,
    const std::vector<KoncludeObjectPropertyAssertion> &retractProperty) {
  std::vector<ClassFact> classLists[2];
  std::vector<PropertyFact> propertyLists[2];
  for (const KoncludeClassAssertion &a : tellClass) {
    classLists[0].push_back(ClassFact(a.individualIRI, a.classIRI));
  }
  for (const KoncludeClassAssertion &a : retractClass) {
    classLists[1].push_back(ClassFact(a.individualIRI, a.classIRI));
  }
  for (const KoncludeObjectPropertyAssertion &a : tellProperty) {
    propertyLists[0].push_back(PropertyFact(a.subjectIRI, a.propertyIRI, a.objectIRI));
  }
  for (const KoncludeObjectPropertyAssertion &a : retractProperty) {
    propertyLists[1].push_back(PropertyFact(a.subjectIRI, a.propertyIRI, a.objectIRI));
  }
  return sendTellAndRetract(classLists[0], classLists[1], propertyLists[0],
                            propertyLists[1]);
}

bool KoncludeWorkerClient::sendTellAndRetract(
    const std::vector<ClassFact> &tellClass,
    const std::vector<ClassFact> &retractClass,
    const std::vector<PropertyFact> &tellProperty,
    const std::vector<PropertyFact> &retractProperty) {
  if (!mOntologyLoaded) {
    mLastError = "no ontology loaded";
    return false;
  }
  if (mRestartEvery > 0 && mChangesOnWorker >= mRestartEvery && !restartWorker()) {
    return false;
  }
  Frame request{konclude_worker::kTellAndRetract, {}};
  addItem(request, std::to_string(tellClass.size()));
  addItem(request, std::to_string(retractClass.size()));
  addItem(request, std::to_string(tellProperty.size()));
  addItem(request, std::to_string(retractProperty.size()));
  for (const std::vector<ClassFact> *list : {&tellClass, &retractClass}) {
    for (const ClassFact &fact : *list) {
      addItem(request, fact.first);
      addItem(request, fact.second);
    }
  }
  for (const std::vector<PropertyFact> *list : {&tellProperty, &retractProperty}) {
    for (const PropertyFact &fact : *list) {
      addItem(request, std::get<0>(fact));
      addItem(request, std::get<1>(fact));
      addItem(request, std::get<2>(fact));
    }
  }
  Frame response;
  if (!roundTrip(request, &response)) {
    return false;
  }
  if (response.kind != konclude_worker::kOk) {
    mLastError = response.items.empty() ? "tell/retract failed" : response.items[0].value;
    return false;
  }
  // track the state the worker now has (for re-telling after a restart)
  for (const ClassFact &fact : retractClass) {
    mClassFacts.erase(fact);
  }
  for (const ClassFact &fact : tellClass) {
    mClassFacts.insert(fact);
  }
  for (const PropertyFact &fact : retractProperty) {
    mPropertyFacts.erase(fact);
  }
  for (const PropertyFact &fact : tellProperty) {
    mPropertyFacts.insert(fact);
  }
  ++mChangesOnWorker;
  return true;
}

bool KoncludeWorkerClient::query(const std::string &sparql) {
  mVariableNames.clear();
  mBindings.clear();
  mBindingNull.clear();
  mRows = 0;
  Frame request{konclude_worker::kQuery, {}};
  addItem(request, sparql);
  Frame response;
  if (!roundTrip(request, &response)) {
    return false;
  }
  if (response.kind != konclude_worker::kOk || response.items.size() < 2) {
    mLastError = response.items.empty() ? "query failed" : response.items[0].value;
    return false;
  }
  int variables = std::atoi(response.items[0].value.c_str());
  int rows = std::atoi(response.items[1].value.c_str());
  if (variables < 0 || rows < 0 ||
      response.items.size() != 2 + static_cast<size_t>(variables) * (1 + rows)) {
    mLastError = "malformed query response";
    return false;
  }
  for (int v = 0; v < variables; ++v) {
    mVariableNames.push_back(response.items[2 + v].value);
  }
  for (size_t i = 2 + variables; i < response.items.size(); ++i) {
    mBindings.push_back(response.items[i].value);
    mBindingNull.push_back(response.items[i].isNull);
  }
  mRows = rows;
  return true;
}

int KoncludeWorkerClient::resultRowCount() const { return mRows; }

int KoncludeWorkerClient::resultVariableCount() const {
  return static_cast<int>(mVariableNames.size());
}

const char *KoncludeWorkerClient::resultVariableName(int varIndex) const {
  if (varIndex < 0 || varIndex >= resultVariableCount()) {
    return nullptr;
  }
  return mVariableNames[varIndex].c_str();
}

const char *KoncludeWorkerClient::resultBinding(int row, int varIndex) const {
  if (row < 0 || row >= mRows || varIndex < 0 || varIndex >= resultVariableCount()) {
    return nullptr;
  }
  size_t index = static_cast<size_t>(row) * mVariableNames.size() + varIndex;
  return mBindingNull[index] ? nullptr : mBindings[index].c_str();
}

bool KoncludeWorkerClient::checkConsistency(bool *consistent) {
  Frame request{konclude_worker::kCheckConsistency, {}};
  Frame response;
  if (!roundTrip(request, &response)) {
    return false;
  }
  if (response.kind != konclude_worker::kOk || response.items.size() != 1) {
    mLastError = response.items.empty() ? "check failed" : response.items[0].value;
    return false;
  }
  *consistent = response.items[0].value == "1";
  return true;
}

bool KoncludeWorkerClient::checkSatisfiability(const std::string &classIRI,
                                               bool *satisfiable) {
  Frame request{konclude_worker::kCheckSatisfiable, {}};
  addItem(request, classIRI);
  Frame response;
  if (!roundTrip(request, &response)) {
    return false;
  }
  if (response.kind != konclude_worker::kOk || response.items.size() != 1) {
    mLastError = response.items.empty() ? "check failed" : response.items[0].value;
    return false;
  }
  *satisfiable = response.items[0].value == "1";
  return true;
}

const char *KoncludeWorkerClient::lastError() const { return mLastError.c_str(); }

pid_t KoncludeWorkerClient::workerPid() const { return mPid; }

long KoncludeWorkerClient::workerRssKb() const {
  if (mPid <= 0) {
    return -1;
  }
  std::string path = "/proc/" + std::to_string(mPid) + "/status";
  FILE *f = std::fopen(path.c_str(), "r");
  if (!f) {
    return -1;
  }
  char line[256];
  long value = -1;
  while (std::fgets(line, sizeof(line), f)) {
    if (std::strncmp(line, "VmRSS:", 6) == 0) {
      value = std::atol(line + 6);
      break;
    }
  }
  std::fclose(f);
  return value;
}

int KoncludeWorkerClient::restartCount() const { return mRestarts; }
