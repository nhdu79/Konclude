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

#include "Parser/Expressions/CClassAssertionExpression.h"
#include "Parser/Expressions/CClassExpression.h"
#include "Parser/Expressions/CNamedIndividualExpression.h"

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
    : mInstanceManager(instanceManager), mOntologyLoaded(false) {}

CEmbeddedOntologyLoader::~CEmbeddedOntologyLoader() {}

bool CEmbeddedOntologyLoader::loadOntologyFile(const QString &filePath) {
  if (!mInstanceManager->isReady()) {
    // mLastError already set in CEmbeddedReasoner's constructor if this
    // happened.
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
  mInstanceManager->getPreconditionSynchronizer()->delegateCommand(
      createKBCommand);
  CCommandExecutedBlocker::waitExecutedCommand(createKBCommand);

  QStringList ontoIRIList;
  ontoIRIList.append(filePath);
  CLoadKnowledgeBaseOWLAutoOntologyCommand *loadKBCommand =
      new CLoadKnowledgeBaseOWLAutoOntologyCommand(
          mInstanceManager->getKnowledgeBaseName(), ontoIRIList);
  mInstanceManager->getOwlLinkProcessor()->delegateCommand(loadKBCommand);
  CCommandExecutedBlocker::waitExecutedCommand(loadKBCommand);

  // No structured success/failure signal exists on these commands (see
  // docs/EMBEDDED_INTERFACE.md) -- failures surface only via
  // the ERROR-level log messages captured by
  // CEmbeddedReasoner::postLogMessage().
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
    mInstanceManager->getPreconditionSynchronizer()->delegateCommand(
        revCommand);
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
    // docs/EMBEDDED_INTERFACE.md.
    delete scratchRev;
    ++completed;

    fprintf(stderr, "[probe] iteration %d/%d: done.\n", i + 1, iterations);
    fflush(stderr);
  }
  return completed;
}

bool CEmbeddedOntologyLoader::addAssertionAxioms(
    CConcreteOntologyUpdateCollectorBuilder *builder,
    const ClassAssertionFact *assertions, int assertionCount, bool tell) {
  for (int i = 0; i < assertionCount; ++i) {
    const ClassAssertionFact &fact = assertions[i];
    CNamedIndividualExpression *indiExp =
        builder->getNamedIndividual(fact.individualIRI);
    CClassExpression *classExp = builder->getClass(fact.classIRI);
    CClassAssertionExpression *axiom =
        (indiExp && classExp) ? builder->getClassAssertion(indiExp, classExp)
                              : nullptr;
    if (!axiom) {
      mInstanceManager->setLastError(
          QString("Failed to build ClassAssertion(%1, %2).")
              .arg(fact.individualIRI)
              .arg(fact.classIRI));
      return false;
    }
    if (tell) {
      builder->tellOntologyAxiom(axiom);
    } else {
      builder->retractOntologyAxiom(axiom);
    }
  }
  return true;
}

bool CEmbeddedOntologyLoader::tellAndRetractAssertions(
    const ClassAssertionFact *tellAssertions, int tellAssertionCount,
    const ClassAssertionFact *retractAssertions, int retractAssertionCount) {
  if (!mOntologyLoaded) {
    mInstanceManager->setLastError("No ontology loaded.");
    return false;
  }
  if (tellAssertionCount == 0 && retractAssertionCount == 0) {
    return true;
  }

  // Same sequence as an OWLlink <Tell>/<Retract> request
  // (COWLlinkProcessor.cpp, CTellKnowledgeBaseOWL2XMLNodeCommand branch):
  // 1. create a revision on top of the currently installed one,
  CCreateKnowledgeBaseRevisionUpdateCommand *createRevCommand =
      new CCreateKnowledgeBaseRevisionUpdateCommand(
          mInstanceManager->getKnowledgeBaseName());
  mInstanceManager->getPreconditionSynchronizer()->delegateCommand(
      createRevCommand);
  CCommandExecutedBlocker::waitExecutedCommand(createRevCommand);
  COntologyRevision *ontRev = createRevCommand->getOntologyRevision();
  delete createRevCommand;
  if (!ontRev) {
    mInstanceManager->setLastError(
        "Failed to create a new knowledge base revision.");
    return false;
  }

  // 2. build the changes into it (what CParseOWL2XMLOntologyCommand's
  // handler does with the parsed axioms; retracts first, so a fact both
  // retracted and told in one call ends up told),
  CConcreteOntologyUpdateCollectorBuilder *builder =
      new CConcreteOntologyUpdateCollectorBuilder(ontRev->getOntology());
  builder->initializeBuilding();
  bool built =
      addAssertionAxioms(builder, retractAssertions, retractAssertionCount,
                         false) &&
      addAssertionAxioms(builder, tellAssertions, tellAssertionCount, true);
  builder->completeBuilding();
  delete builder;
  if (!built) {
    // Never installed -- the revision (and its ontology) is still ours.
    delete ontRev;
    return false;
  }

  // 3. install it as the knowledge base's current revision. Reasoning is
  // lazy: nothing is (re-)computed until the next query.
  CInstallKnowledgeBaseRevisionUpdateCommand *installCommand =
      new CInstallKnowledgeBaseRevisionUpdateCommand(
          mInstanceManager->getKnowledgeBaseName(), ontRev);
  mInstanceManager->getPreconditionSynchronizer()->delegateCommand(
      installCommand);
  CCommandExecutedBlocker::waitExecutedCommand(installCommand);
  delete installCommand;
  return true;
}

bool CEmbeddedOntologyLoader::isOntologyLoaded() const {
  return mOntologyLoaded;
}

}; // end namespace Embedded

}; // end namespace Interface

}; // end namespace Control

}; // end namespace Konclude

#endif // KONCLUDE_COMPILE_EMBEDDED_INTERFACE
