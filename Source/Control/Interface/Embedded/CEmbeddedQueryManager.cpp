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

#include "CEmbeddedQueryManager.h"

#include "CEmbeddedReasoner.h"
#include "CEmbeddedOntologyLoader.h"

#include "Control/Command/CCommandExecutedBlocker.h"
#include "Control/Command/CPreconditionSynchronizer.h"

#include "Control/Command/Instructions/CCalculateQueryCommand.h"
#include "Control/Command/Instructions/CCreateKnowledgeBaseRevisionUpdateCommand.h"
#include "Control/Command/Instructions/CIsConsistentQueryCommand.h"
#include "Control/Command/Instructions/CKnowledgeBaseQueryCommand.h"
#include "Control/Command/Instructions/CPreprocessKnowledgeBaseRequirementsForQueryCommand.h"
#include "Control/Command/Instructions/CProcessClassNameSatisfiableQueryCommand.h"

#include "Control/Interface/OWLlink/COWLlinkProcessor.h"

#include "Reasoner/Query/CBooleanQueryResult.h"
#include "Reasoner/Query/CQuery.h"
#include "Reasoner/Query/CQueryResult.h"
#include "Reasoner/Query/CVariableBindingsAnswersResult.h"
#include "Reasoner/Query/CVariableBindingsAnswersResultIterator.h"
#include "Reasoner/Query/CVariableBindingsResultIterator.h"

#include "Reasoner/Revision/COntologyRevision.h"

#include "Reasoner/Generator/CConcreteOntologyQueryExtendedBuilder.h"
#include "Reasoner/Generator/CConcreteOntologyUpdateSeparateHashingCollectorBuilder.h"

#include "Parser/CSPARQLSimpleQueryParser.h"

using namespace Konclude::Control::Command;
using namespace Konclude::Control::Command::Instructions;
using namespace Konclude::Control::Interface::OWLlink;
using namespace Konclude::Reasoner::Query;
using namespace Konclude::Reasoner::Revision;
using namespace Konclude::Reasoner::Generator;
using namespace Konclude::Reasoner::Ontology;
using namespace Konclude::Parser;

namespace Konclude {

namespace Control {

namespace Interface {

namespace Embedded {

CEmbeddedQueryManager::CEmbeddedQueryManager(
    CEmbeddedReasoner *instanceManager, CEmbeddedOntologyLoader *ontologyLoader)
    : mInstanceManager(instanceManager), mOntologyLoader(ontologyLoader),
      mCurrentQueryRevision(nullptr) {}

CEmbeddedQueryManager::~CEmbeddedQueryManager() {
  // mCurrentQueryRevision is never installed regardless of state install
  // status -- always a plain delete.
  delete mCurrentQueryRevision;
}

void CEmbeddedQueryManager::resetForNewState() {
  delete mCurrentQueryRevision;
  mCurrentQueryRevision = nullptr;
}

bool CEmbeddedQueryManager::extractBooleanResult(
    CKnowledgeBaseQueryCommand *command, bool *resultOut) {
  if (!command || !resultOut) {
    mInstanceManager->setLastError("Internal error: missing command or output pointer.");
    return false;
  }
  CCalculateQueryCommand *calcCommand = command->getCalculateQueryCommand();
  if (!calcCommand) {
    mInstanceManager->setLastError("Query produced no calculate-query sub-command.");
    return false;
  }
  CQuery *query = calcCommand->getQuery();
  if (!query) {
    mInstanceManager->setLastError("Query object missing from calculate-query command.");
    return false;
  }
  CQueryResult *result = query->getQueryResult();
  CBooleanQueryResult *boolResult = dynamic_cast<CBooleanQueryResult *>(result);
  if (!boolResult) {
    mInstanceManager->setLastError("Query did not produce a boolean result.");
    return false;
  }
  *resultOut = boolResult->getResult();
  return true;
}

bool CEmbeddedQueryManager::checkConsistency(bool *consistentOut) {
  if (!mOntologyLoader->isOntologyLoaded()) {
    mInstanceManager->setLastError("No ontology loaded.");
    return false;
  }
  CIsConsistentQueryCommand *command =
      new CIsConsistentQueryCommand(mInstanceManager->getKnowledgeBaseName());
  mInstanceManager->getOwlLinkProcessor()->delegateCommand(command);
  CCommandExecutedBlocker::waitExecutedCommand(command);
  return extractBooleanResult(command, consistentOut);
}

bool CEmbeddedQueryManager::checkSatisfiability(const QString &classIRI,
                                                bool *satisfiableOut) {
  if (!mOntologyLoader->isOntologyLoaded()) {
    mInstanceManager->setLastError("No ontology loaded.");
    return false;
  }
  CProcessClassNameSatisfiableQueryCommand *command =
      new CProcessClassNameSatisfiableQueryCommand(
          mInstanceManager->getKnowledgeBaseName(), classIRI);
  mInstanceManager->getOwlLinkProcessor()->delegateCommand(command);
  CCommandExecutedBlocker::waitExecutedCommand(command);
  return extractBooleanResult(command, satisfiableOut);
}

bool CEmbeddedQueryManager::executeConjunctiveQuery(
    const QString &sparqlSelectQuery) {
  mLastQueryVariableNames.clear();
  mLastQueryResultRows.clear();

  if (!mOntologyLoader->isOntologyLoaded()) {
    mInstanceManager->setLastError("No ontology loaded.");
    return false;
  }
  if (!mOntologyLoader->hasCurrentState()) {
    mInstanceManager->setLastError("No current state; call beginNewState() first.");
    return false;
  }

  // See CEmbeddedOntologyLoader::ensureCurrentStateInstalled() for why this
  // lazy, once-per-state install is NOT optional.
  mOntologyLoader->ensureCurrentStateInstalled();

  // Create a query-layer revision on top of the now-installed state ONCE
  // per FD state, on the first query, and REUSE it for every subsequent
  // query against the same state -- mirrors COWLlinkProcessor::
  // processCustomsEvents' CParseProcessSPARQLTextCommand handling, which
  // does exactly this: `lastGetCurrKBRevC` (the fresh revision) is created
  // once and shared across every consecutive SPARQL_QUERY operation in a
  // batch, only reset on an intervening update. Querying the installed
  // state revision directly, with no extra layer at all, does NOT work:
  // confirmed empirically -- pre-existing ABox content resolves fine that
  // way, but facts asserted via CEmbeddedOntologyLoader::assertClassFact()
  // in the same session silently return zero rows (the historical bug this
  // comment used to describe, but now proven to be about newly-Told content
  // specifically, not about install status, which the loader's
  // ensureCurrentStateInstalled() already covers). A layer is required; it
  // just does not need to be a NEW one per query. See
  // docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md's Decision 5 for why
  // per-query layers were expensive enough to matter (one leaked
  // CRealizerThread per query) and Decision 6 for this fix.
  if (!mCurrentQueryRevision) {
    CCreateKnowledgeBaseRevisionUpdateCommand *createRevCommand =
        new CCreateKnowledgeBaseRevisionUpdateCommand(
            mInstanceManager->getKnowledgeBaseName());
    mInstanceManager->getPreconditionSynchronizer()->delegateCommand(createRevCommand);
    CCommandExecutedBlocker::waitExecutedCommand(createRevCommand);
    mCurrentQueryRevision = createRevCommand->getOntologyRevision();
    delete createRevCommand;
    if (!mCurrentQueryRevision) {
      mInstanceManager->setLastError(
          "Failed to create a new knowledge base revision for querying.");
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
    mInstanceManager->setLastError(
        QString("Expected exactly one query from the given SPARQL text, got "
                "%1; only a single SELECT with one basic graph pattern is "
                "supported (see docs/CONJUNCTIVE_QUERY_PIPELINE.md).")
            .arg(queryList.size()));
    qDeleteAll(queryList);
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
  mInstanceManager->getPreconditionSynchronizer()->delegateCommand(prepQueryCommand);
  CCommandExecutedBlocker::waitExecutedCommand(prepQueryCommand);
  delete prepQueryCommand;

  CCalculateQueryCommand *calcQueryCommand = new CCalculateQueryCommand(query);
  mInstanceManager->getPreconditionSynchronizer()->delegateCommand(calcQueryCommand);
  CCommandExecutedBlocker::waitExecutedCommand(calcQueryCommand);
  delete calcQueryCommand;

  // query itself (built fresh from the SPARQL text above, unlike
  // mCurrentQueryRevision which is reused across calls) is deleted below,
  // once its result has been copied out into plain Qt containers -- see
  // CJNIQueryProcessor's queryOntology*() methods, which delete their own
  // freshly-built CQuery-derived objects (e.g. `delete instancesQuery;`)
  // the same way after extracting results via a visitor callback.
  CQueryResult *result = query->getQueryResult();
  CVariableBindingsAnswersResult *varBindAnsRes =
      dynamic_cast<CVariableBindingsAnswersResult *>(result);
  if (!varBindAnsRes) {
    mInstanceManager->setLastError("Query did not produce a variable-bindings result.");
    delete query;
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
  delete query;

  // ontRev (== mCurrentQueryRevision) is NOT deleted here -- it is reused
  // for subsequent queries against this same state and only freed by
  // resetForNewState()/the destructor.

  return true;
}

int CEmbeddedQueryManager::getLastQueryResultRowCount() {
  return mLastQueryResultRows.size();
}

int CEmbeddedQueryManager::getLastQueryResultVariableCount() {
  return mLastQueryVariableNames.size();
}

const char *
CEmbeddedQueryManager::getLastQueryResultVariableNameCStr(int varIndex) {
  if (varIndex < 0 || varIndex >= mLastQueryVariableNames.size()) {
    mLastQueryResultStringUtf8 = QByteArray();
  } else {
    mLastQueryResultStringUtf8 = mLastQueryVariableNames.at(varIndex).toUtf8();
  }
  return mLastQueryResultStringUtf8.constData();
}

const char *CEmbeddedQueryManager::getLastQueryResultBindingCStr(int row,
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
