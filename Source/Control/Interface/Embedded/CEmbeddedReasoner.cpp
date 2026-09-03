/*
 *		Copyright (C) 2013-2015, 2019 by the Konclude Developer Team.
 *
 *		This file is part of the reasoning system Konclude.
 *		For details and support, see <http://konclude.com/>.
 *
 *		Konclude is free software: you can redistribute it and/or modify
 *		it under the terms of version 3 of the GNU Lesser General Public
 *		License (LGPLv3) as published by the Free Software Foundation.
 *
 *		Konclude is distributed in the hope that it will be useful,
 *		but WITHOUT ANY WARRANTY; without even the implied warranty of
 *		MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *		GNU (Lesser) General Public License for more details.
 *
 *		You should have received a copy of the GNU (Lesser) General
 * Public License along with Konclude. If not, see
 * <http://www.gnu.org/licenses/>.
 *
 */

#ifdef KONCLUDE_COMPILE_EMBEDDED_INTERFACE

#include "CEmbeddedReasoner.h"

#include <QCoreApplication>

#include "Utilities/CSingletonProvider.hpp"

#include "Control/Command/CCommanderManagerConfigType.h"
#include "Control/Command/CCommanderManagerThread.h"
#include "Control/Command/CDefaultCommanderInitializationFactory.h"
#include "Control/Command/CReasonerConfigurationGroup.h"

#include "Control/Command/Instructions/CInitializeConfigurationCommand.h"
#include "Control/Command/Instructions/CInitializeReasonerCommand.h"

#include "CEmbeddedOWLlinkProcessor.h"
#include "CEmbeddedOntologyLoader.h"
#include "CEmbeddedQueryManager.h"

using namespace Konclude::Utilities;

namespace Konclude {

namespace Control {

namespace Interface {

namespace Embedded {

namespace {
// The QCoreApplication instance is process-global (Konclude requires
// exactly one per process) -- guard it so repeated CEmbeddedReasoner
// create/destroy cycles within one process don't try to construct a
// second one. See docs/FASTDOWNWARD_EMBEDDING.md phase 3.
QCoreApplication *gEmbeddedQtApp = nullptr;

// The known startup deadlock is triggered by exactly 1 processing
// thread; never let this drop to 1 regardless of caller/environment.
// NOTE: "Konclude.Calculation.ProcessorCount" is registered by
// CReasonerConfigurationGroup's own constructor with a *string*
// default of "1" (Control/Command/CReasonerConfigurationGroup.cpp) --
// not an integer, and re-registering the name via addConfigProperty()
// (tried first) silently created a second, differently-typed,
// unread description rather than overriding the real one, reproducing
// the exact single-thread deadlock this is meant to prevent. The
// correct mechanism is a per-CConfiguration override via
// createAndSetConfig()+readFromString(), mirroring exactly what the
// CLI's "-w N" flag does (CCommandLineLoader::setConfiguration()) --
// applied per-instance in the constructor below, not once globally.
const char *cMinimumProcessorCountString = "2";
} // namespace

void CEmbeddedReasoner::ensureQCoreApplication() {
  if (QCoreApplication::instance() == nullptr) {
    static int argc = 1;
    static char appName[] = "Konclude";
    static char *argv[] = {appName, nullptr};
    // Deliberately never call exec() -- see docs/FASTDOWNWARD_EMBEDDING.md
    // phase 3: the JNI interface already proves Konclude's synchronous
    // command completion (CBlockingCallbackData / CCommandExecutedBlocker)
    // does not depend on a running Qt event loop.
    gEmbeddedQtApp = new QCoreApplication(argc, argv);
  }
}

CEmbeddedReasoner::CEmbeddedReasoner()
    : mConfig(nullptr), mReasonerCommander(nullptr),
      mPreconditionSynchronizer(nullptr), mOwlLinkProcessor(nullptr),
      mOntologyLoader(nullptr), mQueryManager(nullptr) {
  ensureQCoreApplication();

  CLogger::getInstance()->addLogObserver(this, 70.0);

  CConfigurationGroup *reasonerConfigGroup =
      CSingletonProvider<CReasonerConfigurationGroup>::getInstance()
          ->getReferencedConfigurationGroup();
  CConfiguration *rootConfig = new CConfiguration(reasonerConfigGroup);
  mConfig = new CConfiguration(rootConfig);

  // Each instance gets its own CCommanderManager -- own worker threads,
  // own knowledge-base namespace -- so a fixed KB name per instance is
  // fine; there is no cross-instance sharing to collide with.
  mKnowledgeBaseName = QString("http://konclude.com/embedded/kb");

  // Force >=2 processing threads on THIS instance's config, as a
  // per-CConfiguration override -- exactly mirroring what "-w N" does
  // for the CLI (CCommandLineLoader::setConfiguration(), which calls
  // createAndSetConfig()+readFromString() rather than touching the
  // shared group's description). Must happen before
  // CInitializeReasonerCommand below, which is what actually reads it.
  CConfigData *processorCountData =
      mConfig->createAndSetConfig("Konclude.Calculation.ProcessorCount");
  if (processorCountData) {
    processorCountData->readFromString(cMinimumProcessorCountString);
  }

  // Same three overrides CSPARQLFileComandLinePreparationTranslator
  // hardcodes unconditionally for every CLI `sparqlfile`/`sparqlserver`
  // run
  // (Control/Interface/CommandLine/CSPARQLFileComandLinePreparationTranslator.cpp)
  // -- CEmbeddedQueryManager::executeConjunctiveQuery() relies on them, and
  // per that config's own comment, RepresentativePropagation/
  // SignatureMirroringBlocking "are currently not compatible with the
  // query answering techniques". Applied unconditionally here (not just
  // when a CQ is actually run) since, like ProcessorCount above, this
  // must be set before CInitializeReasonerCommand.
  CConfigData *complexQueryingSupportData = mConfig->createAndSetConfig(
      "Konclude.Calculation.Querying.ComplexQueryingSupport");
  if (complexQueryingSupportData) {
    complexQueryingSupportData->readFromString("true");
  }
  CConfigData *representativePropagationData = mConfig->createAndSetConfig(
      "Konclude.Calculation.Optimization.RepresentativePropagation");
  if (representativePropagationData) {
    representativePropagationData->readFromString("false");
  }
  CConfigData *signatureMirroringBlockingData = mConfig->createAndSetConfig(
      "Konclude.Calculation.Optimization.SignatureMirroringBlocking");
  if (signatureMirroringBlockingData) {
    signatureMirroringBlockingData->readFromString("false");
  }

  // CReasonerConfigurationGroup registers "Konclude.Execution.CommanderManager"
  // with a NULL default (CCommanderManagerConfigType(0)) -- on its own,
  // readCommanderManagerConfig() legitimately returns nullptr. The CLI's
  // "-DefaultReasonerLoader" (Control/Loader/CDefaultReasonerLoader.cpp)
  // is what actually constructs a CCommanderManagerThread and wires it in;
  // it is a CLoader we don't otherwise need, so its ~6-line init()/load()
  // sequence is replicated directly here instead of pulling in the
  // CLoader/CLoaderFactory machinery around it.
  //
  // NOTE (multi-instance caveat): CDefaultReasonerLoader writes the
  // manager via group->setConfigDefaultData(...) -- the GROUP's shared
  // default slot, not a per-CConfiguration override (it uses
  // createConfig(), not createAndSetConfig()). That slot is shared
  // across the whole process (CSingletonProvider<CReasonerConfigurationGroup>).
  // Sequential create -> use -> destroy of one CEmbeddedReasoner at a
  // time is fine (this constructor captures its own manager pointer
  // into mReasonerCommander immediately, used via direct method calls
  // afterward, not re-resolved through config). Genuinely concurrent,
  // simultaneously-alive instances are NOT proven safe by this
  // mechanism and need verification before being relied on -- flagged
  // for the phase 7 stress test, and the public header's claim about
  // independent handles has been narrowed until that's verified.
  CCommanderManagerThread *commanderThread = new CCommanderManagerThread();
  CConfigData *commanderConfigData =
      mConfig->createConfig("Konclude.Execution.CommanderManager");
  CCommanderManagerConfigType *commanderConfigType =
      commanderConfigData ? dynamic_cast<CCommanderManagerConfigType *>(
                                commanderConfigData->getConfigType())
                          : nullptr;
  if (commanderConfigType) {
    commanderConfigType->setCommanderManager(commanderThread);
    reasonerConfigGroup->setConfigDefaultData(
        reasonerConfigGroup->getConfigIndex(
            "Konclude.Execution.CommanderManager"),
        commanderConfigData);

    commanderThread->realizeCommand(
        new CInitializeConfigurationCommand(mConfig));
    commanderThread->realizeCommand(new CInitializeReasonerCommand(
        new CDefaultCommanderInitializationFactory()));

    mReasonerCommander = commanderThread;

    // Commands must be delegated through a CPreconditionSynchronizer,
    // not straight to mReasonerCommander -- discovered by reproducing a
    // hang: a CCreateKnowledgeBaseCommand delegated directly to the
    // commander sat forever because nothing was watching for its
    // preconditions (reasoner initialization, itself still in flight via
    // the two realizeCommand() calls above) to become satisfied. Every
    // COWLlinkProcessor-derived class in the codebase routes through
    // exactly this wrapper (its own preSynchronizer member) instead of
    // its underlying delegater directly -- same fix, applied here.
    mPreconditionSynchronizer =
        new CPreconditionSynchronizer(mReasonerCommander);

    // CCreateKnowledgeBaseCommand (in CEmbeddedOntologyLoader) and the CQ
    // path (in CEmbeddedQueryManager) go through mPreconditionSynchronizer/
    // mReasonerCommander directly, which are properly wired in by
    // CInitializeReasonerCommand above. But ontology LOADING
    // (CLoadKnowledgeBaseOWLAutoOntologyCommand) plus consistency/
    // satisfiability checking (CIsConsistentQueryCommand /
    // CProcessClassNameSatisfiableQueryCommand) are only ever handled
    // inside COWLlinkProcessor::processCustomsEvents -- so a
    // COWLlinkProcessor-derived delegater is needed for them.
    // CEmbeddedOWLlinkProcessor (modeled on the proven
    // Control/Interface/JNI/CJNICommandProcessor) fixes that: it reads the
    // same commander back out of mConfig and starts its own worker thread
    // to actually process those three commands.
    mOwlLinkProcessor = new CEmbeddedOWLlinkProcessor(mConfig);
  } else {
    mLastError =
        "Failed to construct CCommanderManager: unexpected config type.";
    delete commanderThread;
  }

  // The ontology-loader and query-manager collaborators are always
  // constructed, even if the infrastructure above failed to set up --
  // each guards its own entry points against that via isReady() (mirrors
  // the single null check this replaces).
  mOntologyLoader = new CEmbeddedOntologyLoader(this);
  mQueryManager = new CEmbeddedQueryManager(this, mOntologyLoader);
}

CEmbeddedReasoner::~CEmbeddedReasoner() {
  CLogger::getInstance()->removeObserverFromAllDomains(this);

  // NOTE: there is no proven precedent anywhere in Konclude's existing
  // code paths for tearing down a CCommanderManager while the process
  // keeps running -- the CLI/OWLlink loaders only ever run once and let
  // process exit reclaim everything. This relies on CCommanderManager's
  // virtual destructor alone. Flagged for the phase 7 stress test
  // (repeated create/use/destroy in a loop) rather than assumed safe.
  delete mQueryManager;
  delete mOntologyLoader;
  delete mOwlLinkProcessor;
  delete mPreconditionSynchronizer;
  delete mReasonerCommander;
  delete mConfig;
}

void CEmbeddedReasoner::postLogMessage(CLogMessage *message) {
  if (message) {
    mLastError = message->getMessage();
  }
}

const char *CEmbeddedReasoner::getLastErrorCStr() {
  mLastErrorUtf8 = mLastError.toUtf8();
  return mLastErrorUtf8.constData();
}

bool CEmbeddedReasoner::isReady() const {
  return mPreconditionSynchronizer != nullptr;
}

CPreconditionSynchronizer *CEmbeddedReasoner::getPreconditionSynchronizer() const {
  return mPreconditionSynchronizer;
}

COWLlinkProcessor *CEmbeddedReasoner::getOwlLinkProcessor() const {
  return mOwlLinkProcessor;
}

const QString &CEmbeddedReasoner::getKnowledgeBaseName() const {
  return mKnowledgeBaseName;
}

void CEmbeddedReasoner::setLastError(const QString &error) {
  mLastError = error;
}

bool CEmbeddedReasoner::loadOntologyFile(const QString &filePath) {
  return mOntologyLoader->loadOntologyFile(filePath);
}

int CEmbeddedReasoner::probeScratchRevisionCycles(int iterations) {
  return mOntologyLoader->probeScratchRevisionCycles(iterations);
}

bool CEmbeddedReasoner::checkConsistency(bool *consistentOut) {
  return mQueryManager->checkConsistency(consistentOut);
}

bool CEmbeddedReasoner::checkSatisfiability(const QString &classIRI,
                                            bool *satisfiableOut) {
  return mQueryManager->checkSatisfiability(classIRI, satisfiableOut);
}

bool CEmbeddedReasoner::beginNewState() {
  // The previous state's query-layer revision (see CEmbeddedQueryManager::
  // executeConjunctiveQuery()) and its scratch ABox revision (see
  // CEmbeddedOntologyLoader::beginNewState()) are independent objects with
  // no data dependency on each other -- which collaborator resets first
  // does not matter, only that both happen every time this is called.
  mQueryManager->resetForNewState();
  return mOntologyLoader->beginNewState();
}

bool CEmbeddedReasoner::assertClassFact(const QString &individualIRI,
                                        const QString &classIRI) {
  return mOntologyLoader->assertClassFact(individualIRI, classIRI);
}

bool CEmbeddedReasoner::executeConjunctiveQuery(
    const QString &sparqlSelectQuery) {
  return mQueryManager->executeConjunctiveQuery(sparqlSelectQuery);
}

int CEmbeddedReasoner::getLastQueryResultRowCount() {
  return mQueryManager->getLastQueryResultRowCount();
}

int CEmbeddedReasoner::getLastQueryResultVariableCount() {
  return mQueryManager->getLastQueryResultVariableCount();
}

const char *
CEmbeddedReasoner::getLastQueryResultVariableNameCStr(int varIndex) {
  return mQueryManager->getLastQueryResultVariableNameCStr(varIndex);
}

const char *CEmbeddedReasoner::getLastQueryResultBindingCStr(int row,
                                                             int varIndex) {
  return mQueryManager->getLastQueryResultBindingCStr(row, varIndex);
}

}; // end namespace Embedded

}; // end namespace Interface

}; // end namespace Control

}; // end namespace Konclude

#endif // KONCLUDE_COMPILE_EMBEDDED_INTERFACE
