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

#include "CEmbeddedChainedQueryManager.h"

#include "CEmbeddedReasoner.h"
#include "CEmbeddedChainedOntologyLoader.h"

#include "Control/Command/CCommandExecutedBlocker.h"
#include "Control/Command/CPreconditionSynchronizer.h"

#include "Control/Command/Instructions/CCalculateQueryCommand.h"
#include "Control/Command/Instructions/CCreateKnowledgeBaseRevisionUpdateCommand.h"
#include "Control/Command/Instructions/CPreprocessKnowledgeBaseRequirementsForQueryCommand.h"

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
using namespace Konclude::Reasoner::Query;
using namespace Konclude::Reasoner::Revision;
using namespace Konclude::Reasoner::Generator;
using namespace Konclude::Reasoner::Ontology;
using namespace Konclude::Parser;

namespace Konclude {

namespace Control {

namespace Interface {

namespace Embedded {

CEmbeddedChainedQueryManager::CEmbeddedChainedQueryManager(
    CEmbeddedReasoner *instanceManager,
    CEmbeddedChainedOntologyLoader *chainedOntologyLoader)
    : mInstanceManager(instanceManager),
      mChainedOntologyLoader(chainedOntologyLoader),
      mCurrentQueryRevision(nullptr) {}

CEmbeddedChainedQueryManager::~CEmbeddedChainedQueryManager() {
  // mCurrentQueryRevision is never installed regardless of state install
  // status -- always a plain delete.
  delete mCurrentQueryRevision;
}

void CEmbeddedChainedQueryManager::resetForNewState() {
  delete mCurrentQueryRevision;
  mCurrentQueryRevision = nullptr;
}

bool CEmbeddedChainedQueryManager::executeConjunctiveQuery(
    const QString &sparqlSelectQuery) {
  mLastQueryVariableNames.clear();
  mLastQueryResultRows.clear();

  if (!mChainedOntologyLoader->hasCurrentState()) {
    mInstanceManager->setLastError(
        "No current chained state; call beginNewChainedState() first.");
    return false;
  }

  // See CEmbeddedChainedOntologyLoader::ensureCurrentRevisionInstalled() --
  // same NOT-optional reasoning as CEmbeddedOntologyLoader::
  // ensureCurrentStateInstalled().
  mChainedOntologyLoader->ensureCurrentRevisionInstalled();

  // Create a query-layer revision on top of the now-installed chained head
  // ONCE, on the first query after the last chained Tell/retract, and
  // REUSE it for every subsequent query until resetForNewState() is called
  // again -- identical mechanism to CEmbeddedQueryManager::
  // executeConjunctiveQuery(), see its doc comment for the full rationale
  // (mirrors COWLlinkProcessor's lastGetCurrKBRevC batching).
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

  // Same non-Rasqal, single-BGP SPARQL engine as CEmbeddedQueryManager::
  // executeConjunctiveQuery() and sparqlfile/sparqlserver -- see
  // docs/CONJUNCTIVE_QUERY_PIPELINE.md §4.
  CConcreteOntologyUpdateSeparateHashingCollectorBuilder *builder =
      new CConcreteOntologyUpdateSeparateHashingCollectorBuilder(onto);
  CConcreteOntologyQueryExtendedBuilder *queryBuilderGen =
      new CConcreteOntologyQueryExtendedBuilder(baseOnt, onto, ontConfig,
                                                builder);
  CSPARQLSimpleQueryParser *sparqlQueryParser =
      new CSPARQLSimpleQueryParser(queryBuilderGen, builder, onto);
  builder->initializeBuilding();
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

  // See CEmbeddedQueryManager::executeConjunctiveQuery()'s doc comment for
  // why this precondition is required, not optional.
  CPreprocessKnowledgeBaseRequirementsForQueryCommand *prepQueryCommand =
      new CPreprocessKnowledgeBaseRequirementsForQueryCommand(onto);
  mInstanceManager->getPreconditionSynchronizer()->delegateCommand(prepQueryCommand);
  CCommandExecutedBlocker::waitExecutedCommand(prepQueryCommand);
  delete prepQueryCommand;

  CCalculateQueryCommand *calcQueryCommand = new CCalculateQueryCommand(query);
  mInstanceManager->getPreconditionSynchronizer()->delegateCommand(calcQueryCommand);
  CCommandExecutedBlocker::waitExecutedCommand(calcQueryCommand);
  delete calcQueryCommand;

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
  // for subsequent queries against this same chained state and only freed
  // by resetForNewState()/the destructor.

  return true;
}

int CEmbeddedChainedQueryManager::getLastQueryResultRowCount() {
  return mLastQueryResultRows.size();
}

int CEmbeddedChainedQueryManager::getLastQueryResultVariableCount() {
  return mLastQueryVariableNames.size();
}

const char *
CEmbeddedChainedQueryManager::getLastQueryResultVariableNameCStr(int varIndex) {
  if (varIndex < 0 || varIndex >= mLastQueryVariableNames.size()) {
    mLastQueryResultStringUtf8 = QByteArray();
  } else {
    mLastQueryResultStringUtf8 = mLastQueryVariableNames.at(varIndex).toUtf8();
  }
  return mLastQueryResultStringUtf8.constData();
}

const char *CEmbeddedChainedQueryManager::getLastQueryResultBindingCStr(int row,
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
