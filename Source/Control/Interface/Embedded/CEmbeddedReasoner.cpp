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
#include <QFileInfo>

#include <cstdio>

#include "Utilities/CSingletonProvider.hpp"

#include "Control/Command/CCommandExecutedBlocker.h"
#include "Control/Command/CCommanderManagerConfigType.h"
#include "Control/Command/CCommanderManagerThread.h"
#include "Control/Command/CConfigManagerReader.h"
#include "Control/Command/CDefaultCommanderInitializationFactory.h"
#include "Control/Command/CReasonerConfigurationGroup.h"

#include "Control/Command/Instructions/CCalculateQueryCommand.h"
#include "Control/Command/Instructions/CCreateKnowledgeBaseCommand.h"
#include "Control/Command/Instructions/CCreateKnowledgeBaseRevisionUpdateCommand.h"
#include "Control/Command/Instructions/CInstallKnowledgeBaseRevisionUpdateCommand.h"
#include "Control/Command/Instructions/CInitializeConfigurationCommand.h"
#include "Control/Command/Instructions/CInitializeReasonerCommand.h"
#include "Control/Command/Instructions/CIsConsistentQueryCommand.h"
#include "Control/Command/Instructions/CLoadKnowledgeBaseOWLAutoOntologyCommand.h"
#include "Control/Command/Instructions/CPreprocessKnowledgeBaseRequirementsForQueryCommand.h"
#include "Control/Command/Instructions/CProcessClassNameSatisfiableQueryCommand.h"

#include "CEmbeddedOWLlinkProcessor.h"

#include "Reasoner/Query/CBooleanQueryResult.h"
#include "Reasoner/Query/CQuery.h"
#include "Reasoner/Query/CQueryResult.h"
#include "Reasoner/Query/CVariableBindingsAnswersResult.h"
#include "Reasoner/Query/CVariableBindingsAnswersResultIterator.h"
#include "Reasoner/Query/CVariableBindingsResultIterator.h"

#include "Reasoner/Revision/COntologyRevision.h"

#include "Reasoner/Generator/CConcreteOntologyQueryExtendedBuilder.h"
#include "Reasoner/Generator/CConcreteOntologyUpdateSeparateHashingCollectorBuilder.h"
#include "Reasoner/Generator/CConcreteOntologyUpdateCollectorBuilder.h"

#include "Parser/CSPARQLSimpleQueryParser.h"
#include "Parser/Expressions/CNamedIndividualExpression.h"
#include "Parser/Expressions/CClassExpression.h"
#include "Parser/Expressions/CClassAssertionExpression.h"

using namespace Konclude::Utilities;
using namespace Konclude::Reasoner::Query;
using namespace Konclude::Reasoner::Revision;
using namespace Konclude::Reasoner::Generator;
using namespace Konclude::Reasoner::Ontology;
using namespace Konclude::Parser;

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
      mOntologyLoaded(false), mCurrentStateRevision(nullptr),
      mCurrentStateInstalled(false), mCurrentQueryRevision(nullptr) {
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
  // -- executeConjunctiveQuery() below relies on them, and per that
  // config's own comment, RepresentativePropagation/
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

    // CCreateKnowledgeBaseCommand above (and CQuery-command handling in
    // executeConjunctiveQuery()) go through CSPOntologyRevisionManager /
    // CCommanderManagerThread::processCustomsEvents directly, which are
    // properly wired in by CInitializeReasonerCommand above. But ontology
    // LOADING (CLoadKnowledgeBaseOWLAutoOntologyCommand) plus consistency/
    // satisfiability checking (CIsConsistentQueryCommand /
    // CProcessClassNameSatisfiableQueryCommand) are only ever handled
    // inside COWLlinkProcessor::processCustomsEvents -- and nothing here
    // previously instantiated a COWLlinkProcessor-derived delegater for
    // them at all, so those commands were dispatched but never processed,
    // hanging CCommandExecutedBlocker::waitExecutedCommand() forever (see
    // docs/EMBEDDED_CQ_DRIVER.md's former "Known blocker" section).
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
}

CEmbeddedReasoner::~CEmbeddedReasoner() {
  CLogger::getInstance()->removeObserverFromAllDomains(this);

  // NOTE: there is no proven precedent anywhere in Konclude's existing
  // code paths for tearing down a CCommanderManager while the process
  // keeps running -- the CLI/OWLlink loaders only ever run once and let
  // process exit reclaim everything. This relies on CCommanderManager's
  // virtual destructor alone. Flagged for the phase 7 stress test
  // (repeated create/use/destroy in a loop) rather than assumed safe.
  // Only delete if never installed -- once installed, CSPOntologyRevisionManager's
  // onRevContainer owns it and frees it itself (qDeleteAll in its own
  // destructor, reached below via mReasonerCommander's teardown); deleting
  // it here too would be a double-free.
  if (!mCurrentStateInstalled) {
    delete mCurrentStateRevision;
  }
  // mCurrentQueryRevision is never installed regardless of state install
  // status -- always a plain delete.
  delete mCurrentQueryRevision;
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

bool CEmbeddedReasoner::extractBooleanResult(
    CKnowledgeBaseQueryCommand *command, bool *resultOut) {
  if (!command || !resultOut) {
    mLastError = "Internal error: missing command or output pointer.";
    return false;
  }
  CCalculateQueryCommand *calcCommand = command->getCalculateQueryCommand();
  if (!calcCommand) {
    mLastError = "Query produced no calculate-query sub-command.";
    return false;
  }
  CQuery *query = calcCommand->getQuery();
  if (!query) {
    mLastError = "Query object missing from calculate-query command.";
    return false;
  }
  CQueryResult *result = query->getQueryResult();
  CBooleanQueryResult *boolResult = dynamic_cast<CBooleanQueryResult *>(result);
  if (!boolResult) {
    mLastError = "Query did not produce a boolean result.";
    return false;
  }
  *resultOut = boolResult->getResult();
  return true;
}

bool CEmbeddedReasoner::loadOntologyFile(const QString &filePath) {
  if (!mReasonerCommander) {
    // mLastError already set in the constructor if this happened.
    return false;
  }
  if (mOntologyLoaded) {
    mLastError = "Ontology already loaded for this reasoner instance; create a "
                 "new instance to load a different ontology.";
    return false;
  }
  QFileInfo fileInfo(filePath);
  if (!fileInfo.exists()) {
    mLastError = QString("Ontology file does not exist: %1").arg(filePath);
    return false;
  }

  // Mirrors COREBatchProcessingLoader's proven create-then-load command
  // sequence (Source/Control/Loader/COREBatchProcessingLoader.cpp), the
  // same one exercised by the already-verified CLI classification/
  // satisfiability runs -- not hand-derived from the lower-level
  // IRI-resolver machinery.
  CCreateKnowledgeBaseCommand *createKBCommand =
      new CCreateKnowledgeBaseCommand(mKnowledgeBaseName);
  mPreconditionSynchronizer->delegateCommand(createKBCommand);
  CCommandExecutedBlocker::waitExecutedCommand(createKBCommand);

  QStringList ontoIRIList;
  ontoIRIList.append(filePath);
  CLoadKnowledgeBaseOWLAutoOntologyCommand *loadKBCommand =
      new CLoadKnowledgeBaseOWLAutoOntologyCommand(mKnowledgeBaseName,
                                                   ontoIRIList);
  mOwlLinkProcessor->delegateCommand(loadKBCommand);
  CCommandExecutedBlocker::waitExecutedCommand(loadKBCommand);

  // No structured success/failure signal exists on these commands (see
  // docs/FASTDOWNWARD_EMBEDDING.md phase 2) -- failures surface only via
  // the ERROR-level log messages captured by postLogMessage() above.
  mOntologyLoaded = true;
  return true;
}

int CEmbeddedReasoner::probeScratchRevisionCycles(int iterations) {
  if (!mOntologyLoaded) {
    mLastError = "No ontology loaded.";
    return -1;
  }
  int completed = 0;
  for (int i = 0; i < iterations; ++i) {
    fprintf(stderr,
            "[probe] iteration %d/%d: creating scratch revision (not "
            "installing)...\n",
            i + 1, iterations);
    fflush(stderr);

    CCreateKnowledgeBaseRevisionUpdateCommand *revCommand =
        new CCreateKnowledgeBaseRevisionUpdateCommand(mKnowledgeBaseName);
    mPreconditionSynchronizer->delegateCommand(revCommand);
    CCommandExecutedBlocker::waitExecutedCommand(revCommand);

    COntologyRevision *scratchRev = revCommand->getOntologyRevision();
    if (!scratchRev) {
      mLastError =
          QString("Iteration %1: failed to create scratch revision.").arg(i);
      return completed;
    }

    fprintf(stderr,
            "[probe] iteration %d/%d: created, deleting (never installed)...\n",
            i + 1, iterations);
    fflush(stderr);

    // Never installed -- COntologyRevision::~COntologyRevision() deletes
    // its config and CConcreteOntology too (COntologyRevision.cpp:46-49),
    // so this is the whole cleanup, matching the design in
    // docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md's Decision 2.
    delete scratchRev;
    ++completed;

    fprintf(stderr, "[probe] iteration %d/%d: done.\n", i + 1, iterations);
    fflush(stderr);
  }
  return completed;
}

bool CEmbeddedReasoner::checkConsistency(bool *consistentOut) {
  if (!mOntologyLoaded) {
    mLastError = "No ontology loaded.";
    return false;
  }
  CIsConsistentQueryCommand *command =
      new CIsConsistentQueryCommand(mKnowledgeBaseName);
  mOwlLinkProcessor->delegateCommand(command);
  CCommandExecutedBlocker::waitExecutedCommand(command);
  return extractBooleanResult(command, consistentOut);
}

bool CEmbeddedReasoner::checkSatisfiability(const QString &classIRI,
                                            bool *satisfiableOut) {
  if (!mOntologyLoaded) {
    mLastError = "No ontology loaded.";
    return false;
  }
  CProcessClassNameSatisfiableQueryCommand *command =
      new CProcessClassNameSatisfiableQueryCommand(mKnowledgeBaseName,
                                                   classIRI);
  mOwlLinkProcessor->delegateCommand(command);
  CCommandExecutedBlocker::waitExecutedCommand(command);
  return extractBooleanResult(command, satisfiableOut);
}

bool CEmbeddedReasoner::beginNewState() {
  if (!mOntologyLoaded) {
    mLastError = "No ontology loaded.";
    return false;
  }

  // If the previous state was never queried, it was never installed either
  // (see executeConjunctiveQuery()) -- same never-install cleanup
  // probeScratchRevisionCycles() above exercises, a plain delete. If it WAS
  // installed, CSPOntologyRevisionManager's onRevContainer owns it now (see
  // the destructor) -- just drop our pointer to it, do not delete it here.
  if (mCurrentStateRevision && !mCurrentStateInstalled) {
    delete mCurrentStateRevision;
  }
  mCurrentStateRevision = nullptr;
  mCurrentStateInstalled = false;

  // The previous state's query-layer revision (see executeConjunctiveQuery())
  // is never installed regardless -- always a plain delete.
  delete mCurrentQueryRevision;
  mCurrentQueryRevision = nullptr;

  CCreateKnowledgeBaseRevisionUpdateCommand *createRevCommand =
      new CCreateKnowledgeBaseRevisionUpdateCommand(mKnowledgeBaseName);
  mPreconditionSynchronizer->delegateCommand(createRevCommand);
  CCommandExecutedBlocker::waitExecutedCommand(createRevCommand);
  COntologyRevision *ontRev = createRevCommand->getOntologyRevision();
  if (!ontRev) {
    mLastError = "Failed to create a new state (scratch knowledge base revision).";
    return false;
  }

  mCurrentStateRevision = ontRev;
  return true;
}

bool CEmbeddedReasoner::assertClassFact(const QString &individualIRI,
                                        const QString &classIRI) {
  if (!mCurrentStateRevision) {
    mLastError = "No current state; call beginNewState() first.";
    return false;
  }

  CConcreteOntology *onto = mCurrentStateRevision->getOntology();

  // Same CConcreteOntologyUpdateCollectorBuilder + tellOntologyAxiom
  // mechanism COntologyMultiAutoParsingLoader::parseOntology uses for
  // every file-based Tell -- see
  // Control/Interface/JNI/com_konclude_jnibridge_AxiomExpressionBuildingBridge.cpp's
  // buildOWLClassAssertionAxiom for the exact same call sequence driven
  // from outside Konclude's C++ (there via JNI, here via the embedded C
  // API), rather than the untested/unused
  // CCreateKnowledgeBaseRevisionUpdateCommand ABox/query constructor.
  CConcreteOntologyUpdateCollectorBuilder *builder =
      new CConcreteOntologyUpdateCollectorBuilder(onto);
  builder->initializeBuilding();

  bool ok = false;
  CNamedIndividualExpression *indiExp =
      builder->getNamedIndividual(individualIRI);
  CClassExpression *classExp = builder->getClass(classIRI);
  if (indiExp && classExp) {
    CClassAssertionExpression *axiom =
        builder->getClassAssertion(indiExp, classExp);
    if (axiom) {
      ok = builder->tellOntologyAxiom(axiom);
    } else {
      mLastError = QString("Failed to build ClassAssertion(%1, %2).")
                       .arg(individualIRI)
                       .arg(classIRI);
    }
  } else {
    mLastError = QString("Failed to resolve individual '%1' or class '%2'.")
                     .arg(individualIRI)
                     .arg(classIRI);
  }

  builder->completeBuilding();
  delete builder;

  return ok;
}

bool CEmbeddedReasoner::executeConjunctiveQuery(
    const QString &sparqlSelectQuery) {
  mLastQueryVariableNames.clear();
  mLastQueryResultRows.clear();

  if (!mOntologyLoaded) {
    mLastError = "No ontology loaded.";
    return false;
  }
  if (!mCurrentStateRevision) {
    mLastError = "No current state; call beginNewState() first.";
    return false;
  }

  // Install the state's revision, lazily, the first time it's queried --
  // NOT optional. Every existing Tell path in this codebase (see
  // COWLlinkProcessor.cpp's SPARQL_UPDATE_MODIFY handling around line 1133:
  // CCreateKnowledgeBaseRevisionUpdateCommand -> Tell -> immediately
  // CInstallKnowledgeBaseRevisionUpdateCommand) installs a revision before
  // anything ever queries it; nothing in the codebase queries a Told,
  // never-installed revision directly. An earlier attempt at exactly that
  // (forcing OPSBUILD directly on mCurrentStateRevision without installing
  // it first) looked plausible but empirically returned zero rows even for
  // ABox content that predated the Tell -- installing first and then
  // querying via a fresh revision layered on top (matching the proven
  // SPARQL_QUERY handling immediately below that same COWLlinkProcessor.cpp
  // branch) fixes it. This does reintroduce install's costs (onRevContainer
  // retention, O(N) referenceBuildData() copy) -- but only once per FD
  // state, not once per CQ call, which is the actual hot path;
  // see docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md's Decision 2 for the
  // fuller cost analysis and the still-open question of whether that
  // per-state cost is acceptable at FD's scale.
  if (!mCurrentStateInstalled) {
    CInstallKnowledgeBaseRevisionUpdateCommand *installCommand =
        new CInstallKnowledgeBaseRevisionUpdateCommand(mKnowledgeBaseName,
                                                        mCurrentStateRevision);
    mPreconditionSynchronizer->delegateCommand(installCommand);
    CCommandExecutedBlocker::waitExecutedCommand(installCommand);
    mCurrentStateInstalled = true;
  }

  // Create a query-layer revision on top of the now-installed state ONCE
  // per FD state, on the first query, and REUSE it for every subsequent
  // query against the same state -- mirrors COWLlinkProcessor::
  // processCustomsEvents' CParseProcessSPARQLTextCommand handling, which
  // does exactly this: `lastGetCurrKBRevC` (the fresh revision) is created
  // once and shared across every consecutive SPARQL_QUERY operation in a
  // batch, only reset on an intervening update. Querying
  // mCurrentStateRevision directly, with no extra layer at all, does NOT
  // work: confirmed empirically -- pre-existing ABox content resolves fine
  // that way, but facts asserted via assertClassFact() in the same session
  // silently return zero rows (the historical bug this comment used to
  // describe, but now proven to be about newly-Told content specifically,
  // not about install status, which Decision 2 already covers). A layer is
  // required; it just does not need to be a NEW one per query. See
  // docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md's Decision 5 for why
  // per-query layers were expensive enough to matter (one leaked
  // CRealizerThread per query) and Decision 6 for this fix.
  if (!mCurrentQueryRevision) {
    CCreateKnowledgeBaseRevisionUpdateCommand *createRevCommand =
        new CCreateKnowledgeBaseRevisionUpdateCommand(mKnowledgeBaseName);
    mPreconditionSynchronizer->delegateCommand(createRevCommand);
    CCommandExecutedBlocker::waitExecutedCommand(createRevCommand);
    mCurrentQueryRevision = createRevCommand->getOntologyRevision();
    if (!mCurrentQueryRevision) {
      mLastError = "Failed to create a new knowledge base revision for querying.";
      return false;
    }
  }
  COntologyRevision *ontRev = mCurrentQueryRevision;
  CConcreteOntology *onto = ontRev->getOntology();
  COntologyRevision *prevOntRev = ontRev->getPreviousOntologyRevision();
  CConcreteOntology *baseOnt = prevOntRev ? prevOntRev->getOntology() : onto;
  COntologyConfigurationExtension *ontConfig =
      ontRev->getOntologyConfiguration();

  // Build the CQuery* once via the exact same non-Rasqal, single-BGP
  // SPARQL engine sparqlfile/sparqlserver use (see
  // docs/CONJUNCTIVE_QUERY_PIPELINE.md §4 and
  // COWLlinkProcessor::processCustomsEvents' CParseSPARQLQueryCommand
  // branch, which this mirrors) -- KoncludeEmbedded.pro never links
  // Rasqal, so this is the only CQ path available here.
  CConcreteOntologyUpdateSeparateHashingCollectorBuilder *builder =
      new CConcreteOntologyUpdateSeparateHashingCollectorBuilder(onto);
  CConcreteOntologyQueryExtendedBuilder *queryBuilderGen =
      new CConcreteOntologyQueryExtendedBuilder(baseOnt, onto, ontConfig,
                                                builder);
  CSPARQLSimpleQueryParser *sparqlQueryParser =
      new CSPARQLSimpleQueryParser(queryBuilderGen, builder, onto);
  builder->initializeBuilding();
  // NOTE: parseQueryTextList() takes ALREADY-TOKENIZED parts (its loop
  // appends each QStringList entry as one token with no further
  // splitting) -- it is not a "list of full query strings" API despite
  // the name. parseQueryText() is the correct call for one complete,
  // untokenized SPARQL query string; it does its own tokenization via
  // getNextPart() internally (see CSPARQLSimpleQueryParser.cpp).
  sparqlQueryParser->parseQueryText(sparqlSelectQuery);
  builder->completeBuilding();
  QList<CQuery *> queryList = queryBuilderGen->generateQuerys();
  delete sparqlQueryParser;
  delete queryBuilderGen;
  delete builder;

  if (queryList.size() != 1) {
    mLastError =
        QString("Expected exactly one query from the given SPARQL text, got "
                "%1; only a single SELECT with one basic graph pattern is "
                "supported (see docs/CONJUNCTIVE_QUERY_PIPELINE.md).")
            .arg(queryList.size());
    return false;
  }
  CQuery *query = queryList.first();

  // CCalculateQueryCommand's own handler (CCommanderManagerThread::
  // processCustomsEvents) does not trigger this itself -- it just
  // forwards the query straight to the reasoner manager. The proven
  // checkSatisfiability path (COWLlinkProcessor::processCustomsEvents'
  // CProcessClassNameSatisfiableQueryCommand branch) explicitly runs
  // this first, as a precondition, before calculating; without it,
  // CCalculateQueryCommand silently returns zero rows instead of
  // erroring (triples indexing was never brought up to
  // PSCOMPLETELYYPROCESSED/PSSUCESSFULL for this ontology).
  CPreprocessKnowledgeBaseRequirementsForQueryCommand *prepQueryCommand =
      new CPreprocessKnowledgeBaseRequirementsForQueryCommand(onto);
  mPreconditionSynchronizer->delegateCommand(prepQueryCommand);
  CCommandExecutedBlocker::waitExecutedCommand(prepQueryCommand);

  CCalculateQueryCommand *calcQueryCommand = new CCalculateQueryCommand(query);
  mPreconditionSynchronizer->delegateCommand(calcQueryCommand);
  CCommandExecutedBlocker::waitExecutedCommand(calcQueryCommand);

  CQueryResult *result = query->getQueryResult();
  CVariableBindingsAnswersResult *varBindAnsRes =
      dynamic_cast<CVariableBindingsAnswersResult *>(result);
  if (!varBindAnsRes) {
    mLastError = "Query did not produce a variable-bindings result.";
    return false;
  }

  for (const QString &varName : varBindAnsRes->getVariableNames()) {
    mLastQueryVariableNames.append(varName);
  }

  CVariableBindingsResultIterator *cellIt = nullptr;
  CVariableBindingsAnswersResultIterator *rowIt =
      varBindAnsRes->getVariableBindingsAnswersIterator();
  while (rowIt->hasNext()) {
    CVariableBindingsAnswerResult *row = rowIt->getNext();
    cellIt = row->getVariableBindingsIterator(cellIt);
    QStringList rowValues;
    while (cellIt->hasNext()) {
      CVariableBindingResult *cell = cellIt->getNext();
      rowValues.append(cell ? cell->getBindingString() : QString());
    }
    mLastQueryResultRows.append(rowValues);
  }
  delete rowIt;
  delete cellIt;

  // ontRev (== mCurrentQueryRevision) is NOT deleted here -- it is reused
  // for subsequent queries against this same state and only freed by
  // beginNewState()/the destructor.

  return true;
}

int CEmbeddedReasoner::getLastQueryResultRowCount() {
  return mLastQueryResultRows.size();
}

int CEmbeddedReasoner::getLastQueryResultVariableCount() {
  return mLastQueryVariableNames.size();
}

const char *
CEmbeddedReasoner::getLastQueryResultVariableNameCStr(int varIndex) {
  if (varIndex < 0 || varIndex >= mLastQueryVariableNames.size()) {
    mLastQueryResultStringUtf8 = QByteArray();
  } else {
    mLastQueryResultStringUtf8 = mLastQueryVariableNames.at(varIndex).toUtf8();
  }
  return mLastQueryResultStringUtf8.constData();
}

const char *CEmbeddedReasoner::getLastQueryResultBindingCStr(int row,
                                                             int varIndex) {
  if (row < 0 || row >= mLastQueryResultRows.size() || varIndex < 0 ||
      varIndex >= mLastQueryResultRows.at(row).size()) {
    mLastQueryResultStringUtf8 = QByteArray();
  } else {
    mLastQueryResultStringUtf8 =
        mLastQueryResultRows.at(row).at(varIndex).toUtf8();
  }
  return mLastQueryResultStringUtf8.constData();
}

}; // end namespace Embedded

}; // end namespace Interface

}; // end namespace Control

}; // end namespace Konclude

#endif // KONCLUDE_COMPILE_EMBEDDED_INTERFACE
