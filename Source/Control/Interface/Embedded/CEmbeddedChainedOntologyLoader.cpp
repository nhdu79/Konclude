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
 *		You should have received a copy of the GNU (Lesser) General Public
 *		License along with Konclude. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef KONCLUDE_COMPILE_EMBEDDED_INTERFACE

#include "CEmbeddedChainedOntologyLoader.h"

#include "CEmbeddedReasoner.h"

#include "Control/Command/CCommandExecutedBlocker.h"

#include "Control/Command/Instructions/CCreateKnowledgeBaseRevisionUpdateCommand.h"
#include "Control/Command/Instructions/CInstallKnowledgeBaseRevisionUpdateCommand.h"

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

CEmbeddedChainedOntologyLoader::CEmbeddedChainedOntologyLoader(
    CEmbeddedReasoner *instanceManager)
    : mInstanceManager(instanceManager), mCurrentRevision(nullptr),
      mCurrentRevisionInstalled(false) {}

CEmbeddedChainedOntologyLoader::~CEmbeddedChainedOntologyLoader() {
  // Only delete if never installed -- once installed,
  // CSPOntologyRevisionManager's onRevContainer owns it and frees it itself
  // (qDeleteAll in its own destructor, reached via the owning
  // CEmbeddedReasoner's mReasonerCommander teardown); deleting it here too
  // would be a double-free.
  if (!mCurrentRevisionInstalled) {
    delete mCurrentRevision;
  }
}

bool CEmbeddedChainedOntologyLoader::beginNewChainedState() {
  if (mCurrentRevision && !mCurrentRevisionInstalled) {
    delete mCurrentRevision;
  }
  mCurrentRevision = nullptr;
  mCurrentRevisionInstalled = false;

  CCreateKnowledgeBaseRevisionUpdateCommand *createRevCommand =
      new CCreateKnowledgeBaseRevisionUpdateCommand(
          mInstanceManager->getKnowledgeBaseName());
  mInstanceManager->getPreconditionSynchronizer()->delegateCommand(createRevCommand);
  CCommandExecutedBlocker::waitExecutedCommand(createRevCommand);
  COntologyRevision *ontRev = createRevCommand->getOntologyRevision();
  delete createRevCommand;
  if (!ontRev) {
    mInstanceManager->setLastError(
        "Failed to create a new chained state (scratch knowledge base "
        "revision) -- is an ontology loaded?");
    return false;
  }

  mCurrentRevision = ontRev;
  return true;
}

bool CEmbeddedChainedOntologyLoader::tellOrRetractClassFactChained(
    const QString &individualIRI, const QString &classIRI, bool retract) {
  if (!mCurrentRevision) {
    mInstanceManager->setLastError(
        "No current chained state; call beginNewChainedState() first.");
    return false;
  }

  // Mirror COWLlinkProcessor's SPARQL_UPDATE_MODIFY: it always creates its
  // new revision layered on the KB's currently-INSTALLED revision, never a
  // never-installed scratch one. If this is the state's first chained call,
  // the state's own base revision (from beginNewChainedState()) needs
  // installing first, same as a query would otherwise lazily trigger.
  ensureCurrentRevisionInstalled();

  CCreateKnowledgeBaseRevisionUpdateCommand *createRevCommand =
      new CCreateKnowledgeBaseRevisionUpdateCommand(
          mInstanceManager->getKnowledgeBaseName());
  mInstanceManager->getPreconditionSynchronizer()->delegateCommand(createRevCommand);
  CCommandExecutedBlocker::waitExecutedCommand(createRevCommand);
  COntologyRevision *newRev = createRevCommand->getOntologyRevision();
  delete createRevCommand;
  if (!newRev) {
    mInstanceManager->setLastError(
        "Failed to create chained knowledge base revision.");
    return false;
  }

  // A fresh builder tied to newRev's ontology -- every chained call gets a
  // different ontology (that is the whole point), so there is no
  // per-state builder to reuse the way CEmbeddedOntologyLoader::
  // assertClassFact() does.
  CConcreteOntologyUpdateCollectorBuilder *builder =
      new CConcreteOntologyUpdateCollectorBuilder(newRev->getOntology());
  builder->initializeBuilding();

  bool ok = false;
  CNamedIndividualExpression *indiExp =
      builder->getNamedIndividual(individualIRI);
  CClassExpression *classExp = builder->getClass(classIRI);
  if (indiExp && classExp) {
    CClassAssertionExpression *axiom =
        builder->getClassAssertion(indiExp, classExp);
    if (axiom) {
      ok = retract ? builder->retractOntologyAxiom(axiom)
                   : builder->tellOntologyAxiom(axiom);
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
  delete builder;

  if (!ok) {
    // Never installed -- plain delete is the whole cleanup.
    delete newRev;
    return false;
  }

  CInstallKnowledgeBaseRevisionUpdateCommand *installCommand =
      new CInstallKnowledgeBaseRevisionUpdateCommand(
          mInstanceManager->getKnowledgeBaseName(), newRev);
  mInstanceManager->getPreconditionSynchronizer()->delegateCommand(installCommand);
  CCommandExecutedBlocker::waitExecutedCommand(installCommand);
  delete installCommand;

  // The previous mCurrentRevision is already owned by
  // CSPOntologyRevisionManager's onRevContainer (installed above, either
  // just now via ensureCurrentRevisionInstalled() or by an earlier chained
  // call) -- just advance the pointer to the new head of the chain.
  mCurrentRevision = newRev;
  mCurrentRevisionInstalled = true;

  return true;
}

bool CEmbeddedChainedOntologyLoader::assertClassFactChained(
    const QString &individualIRI, const QString &classIRI) {
  return tellOrRetractClassFactChained(individualIRI, classIRI, false);
}

bool CEmbeddedChainedOntologyLoader::retractClassFactChained(
    const QString &individualIRI, const QString &classIRI) {
  return tellOrRetractClassFactChained(individualIRI, classIRI, true);
}

bool CEmbeddedChainedOntologyLoader::hasCurrentState() const {
  return mCurrentRevision != nullptr;
}

COntologyRevision *
CEmbeddedChainedOntologyLoader::ensureCurrentRevisionInstalled() {
  if (!mCurrentRevisionInstalled) {
    CInstallKnowledgeBaseRevisionUpdateCommand *installCommand =
        new CInstallKnowledgeBaseRevisionUpdateCommand(
            mInstanceManager->getKnowledgeBaseName(), mCurrentRevision);
    mInstanceManager->getPreconditionSynchronizer()->delegateCommand(installCommand);
    CCommandExecutedBlocker::waitExecutedCommand(installCommand);
    delete installCommand;
    mCurrentRevisionInstalled = true;
  }
  return mCurrentRevision;
}

}; // end namespace Embedded

}; // end namespace Interface

}; // end namespace Control

}; // end namespace Konclude

#endif // KONCLUDE_COMPILE_EMBEDDED_INTERFACE
