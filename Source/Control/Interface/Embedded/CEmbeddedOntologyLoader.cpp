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

#include "CEmbeddedOntologyLoader.h"

#include <QFileInfo>
#include <cstdio>

#include "CEmbeddedReasoner.h"

#include "Control/Command/CCommandExecutedBlocker.h"

#include "Control/Command/Instructions/CCreateKnowledgeBaseCommand.h"
#include "Control/Command/Instructions/CCreateKnowledgeBaseRevisionUpdateCommand.h"
#include "Control/Command/Instructions/CInstallKnowledgeBaseRevisionUpdateCommand.h"
#include "Control/Command/Instructions/CLoadKnowledgeBaseOWLAutoOntologyCommand.h"

#include "Reasoner/Revision/COntologyRevision.h"

#include "Reasoner/Generator/CConcreteOntologyUpdateCollectorBuilder.h"

#include "Parser/Expressions/CNamedIndividualExpression.h"
#include "Parser/Expressions/CClassExpression.h"
#include "Parser/Expressions/CClassAssertionExpression.h"

using namespace Konclude::Control::Command;
using namespace Konclude::Control::Command::Instructions;
using namespace Konclude::Reasoner::Revision;
using namespace Konclude::Reasoner::Generator;
using namespace Konclude::Reasoner::Ontology;
using namespace Konclude::Parser;
using namespace Konclude::Parser::Expression;

namespace Konclude {

namespace Control {

namespace Interface {

namespace Embedded {

CEmbeddedOntologyLoader::CEmbeddedOntologyLoader(
    CEmbeddedReasoner *instanceManager)
    : mInstanceManager(instanceManager), mOntologyLoaded(false),
      mCurrentStateRevision(nullptr), mCurrentStateInstalled(false),
      mCurrentStateBuilder(nullptr) {}

CEmbeddedOntologyLoader::~CEmbeddedOntologyLoader() {
  // Only delete if never installed -- once installed, CSPOntologyRevisionManager's
  // onRevContainer owns it and frees it itself (qDeleteAll in its own
  // destructor, reached via the owning CEmbeddedReasoner's mReasonerCommander
  // teardown); deleting it here too would be a double-free.
  if (!mCurrentStateInstalled) {
    delete mCurrentStateRevision;
  }
  delete mCurrentStateBuilder;
}

bool CEmbeddedOntologyLoader::loadOntologyFile(const QString &filePath) {
  if (!mInstanceManager->isReady()) {
    // mLastError already set in CEmbeddedReasoner's constructor if this happened.
    return false;
  }
  if (mOntologyLoaded) {
    mInstanceManager->setLastError(
        "Ontology already loaded for this reasoner instance; create a "
        "new instance to load a different ontology.");
    return false;
  }
  QFileInfo fileInfo(filePath);
  if (!fileInfo.exists()) {
    mInstanceManager->setLastError(
        QString("Ontology file does not exist: %1").arg(filePath));
    return false;
  }

  // Mirrors COREBatchProcessingLoader's proven create-then-load command
  // sequence (Source/Control/Loader/COREBatchProcessingLoader.cpp), the
  // same one exercised by the already-verified CLI classification/
  // satisfiability runs -- not hand-derived from the lower-level
  // IRI-resolver machinery.
  CCreateKnowledgeBaseCommand *createKBCommand =
      new CCreateKnowledgeBaseCommand(mInstanceManager->getKnowledgeBaseName());
  mInstanceManager->getPreconditionSynchronizer()->delegateCommand(createKBCommand);
  CCommandExecutedBlocker::waitExecutedCommand(createKBCommand);

  QStringList ontoIRIList;
  ontoIRIList.append(filePath);
  CLoadKnowledgeBaseOWLAutoOntologyCommand *loadKBCommand =
      new CLoadKnowledgeBaseOWLAutoOntologyCommand(
          mInstanceManager->getKnowledgeBaseName(), ontoIRIList);
  mInstanceManager->getOwlLinkProcessor()->delegateCommand(loadKBCommand);
  CCommandExecutedBlocker::waitExecutedCommand(loadKBCommand);

  // No structured success/failure signal exists on these commands (see
  // docs/FASTDOWNWARD_EMBEDDING.md phase 2) -- failures surface only via
  // the ERROR-level log messages captured by CEmbeddedReasoner::postLogMessage().
  mOntologyLoaded = true;
  return true;
}

int CEmbeddedOntologyLoader::probeScratchRevisionCycles(int iterations) {
  if (!mOntologyLoaded) {
    mInstanceManager->setLastError("No ontology loaded.");
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
        new CCreateKnowledgeBaseRevisionUpdateCommand(
            mInstanceManager->getKnowledgeBaseName());
    mInstanceManager->getPreconditionSynchronizer()->delegateCommand(revCommand);
    CCommandExecutedBlocker::waitExecutedCommand(revCommand);

    COntologyRevision *scratchRev = revCommand->getOntologyRevision();
    if (!scratchRev) {
      mInstanceManager->setLastError(
          QString("Iteration %1: failed to create scratch revision.").arg(i));
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

bool CEmbeddedOntologyLoader::beginNewState() {
  if (!mOntologyLoaded) {
    mInstanceManager->setLastError("No ontology loaded.");
    return false;
  }

  // If the previous state was never queried, it was never installed either
  // (see ensureCurrentStateInstalled()) -- same never-install cleanup
  // probeScratchRevisionCycles() above exercises, a plain delete. If it WAS
  // installed, CSPOntologyRevisionManager's onRevContainer owns it now (see
  // the destructor) -- just drop our pointer to it, do not delete it here.
  if (mCurrentStateRevision && !mCurrentStateInstalled) {
    delete mCurrentStateRevision;
  }
  mCurrentStateRevision = nullptr;
  mCurrentStateInstalled = false;

  // The previous state's builder is tied to that state's (now discarded)
  // ontology -- must not survive into the new state.
  delete mCurrentStateBuilder;
  mCurrentStateBuilder = nullptr;

  CCreateKnowledgeBaseRevisionUpdateCommand *createRevCommand =
      new CCreateKnowledgeBaseRevisionUpdateCommand(
          mInstanceManager->getKnowledgeBaseName());
  mInstanceManager->getPreconditionSynchronizer()->delegateCommand(createRevCommand);
  CCommandExecutedBlocker::waitExecutedCommand(createRevCommand);
  COntologyRevision *ontRev = createRevCommand->getOntologyRevision();
  delete createRevCommand;
  if (!ontRev) {
    mInstanceManager->setLastError(
        "Failed to create a new state (scratch knowledge base revision).");
    return false;
  }

  mCurrentStateRevision = ontRev;
  // One builder for this state's ontology, reused by every assertClassFact()
  // call below instead of allocating one per fact -- see mCurrentStateBuilder's
  // doc comment for why this is safe.
  mCurrentStateBuilder =
      new CConcreteOntologyUpdateCollectorBuilder(ontRev->getOntology());
  return true;
}

bool CEmbeddedOntologyLoader::assertClassFact(const QString &individualIRI,
                                              const QString &classIRI) {
  if (!mCurrentStateRevision) {
    mInstanceManager->setLastError("No current state; call beginNewState() first.");
    return false;
  }

  // Same CConcreteOntologyUpdateCollectorBuilder + tellOntologyAxiom
  // mechanism COntologyMultiAutoParsingLoader::parseOntology uses for
  // every file-based Tell -- see
  // Control/Interface/JNI/com_konclude_jnibridge_AxiomExpressionBuildingBridge.cpp's
  // buildOWLClassAssertionAxiom for the exact same call sequence driven
  // from outside Konclude's C++ (there via JNI, here via the embedded C
  // API), rather than the untested/unused
  // CCreateKnowledgeBaseRevisionUpdateCommand ABox/query constructor.
  //
  // mCurrentStateBuilder is one builder shared by every assertClassFact()
  // call for this state (see its doc comment in the header) rather than a
  // fresh one per call; initializeBuilding()/completeBuilding() still
  // bracket each individual Tell exactly as before.
  CConcreteOntologyUpdateCollectorBuilder *builder = mCurrentStateBuilder;
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
      mInstanceManager->setLastError(
          QString("Failed to build ClassAssertion(%1, %2).")
              .arg(individualIRI)
              .arg(classIRI));
    }
  } else {
    mInstanceManager->setLastError(
        QString("Failed to resolve individual '%1' or class '%2'.")
            .arg(individualIRI)
            .arg(classIRI));
  }

  builder->completeBuilding();

  return ok;
}

bool CEmbeddedOntologyLoader::isOntologyLoaded() const {
  return mOntologyLoaded;
}

bool CEmbeddedOntologyLoader::hasCurrentState() const {
  return mCurrentStateRevision != nullptr;
}

COntologyRevision *CEmbeddedOntologyLoader::ensureCurrentStateInstalled() {
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
  // querying via a fresh revision layered on top (see
  // CEmbeddedQueryManager::executeConjunctiveQuery(), which matches the
  // proven SPARQL_QUERY handling immediately below that same
  // COWLlinkProcessor.cpp branch) fixes it. This does reintroduce install's
  // costs (onRevContainer retention, O(N) referenceBuildData() copy) -- but
  // only once per FD state, not once per CQ call, which is the actual hot
  // path; see docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md's Decision 2 for
  // the fuller cost analysis and the still-open question of whether that
  // per-state cost is acceptable at FD's scale.
  if (!mCurrentStateInstalled) {
    CInstallKnowledgeBaseRevisionUpdateCommand *installCommand =
        new CInstallKnowledgeBaseRevisionUpdateCommand(
            mInstanceManager->getKnowledgeBaseName(), mCurrentStateRevision);
    mInstanceManager->getPreconditionSynchronizer()->delegateCommand(installCommand);
    CCommandExecutedBlocker::waitExecutedCommand(installCommand);
    delete installCommand;
    mCurrentStateInstalled = true;
  }
  return mCurrentStateRevision;
}

}; // end namespace Embedded

}; // end namespace Interface

}; // end namespace Control

}; // end namespace Konclude

#endif // KONCLUDE_COMPILE_EMBEDDED_INTERFACE
