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

#include "CEmbeddedOntologyLoader.h"
#include "CEmbeddedReasoner.h"

#include "Control/Command/CCommandExecutedBlocker.h"
#include "Control/Command/CPreconditionSynchronizer.h"

#include "Control/Command/Instructions/CCalculateQueryCommand.h"
#include "Control/Command/Instructions/CCreateKnowledgeBaseRevisionUpdateCommand.h"
#include "Control/Command/Instructions/CIsConsistentQueryCommand.h"
#include "Control/Command/Instructions/CKnowledgeBaseQueryCommand.h"
#include "Control/Command/Instructions/CPreprocessKnowledgeBaseRequirementsForQueryCommand.h"
#include "Control/Command/Instructions/CProcessClassNameSatisfiableQueryCommand.h"
#include "Control/Command/Instructions/CReleaseOntologyCommand.h"

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
  if (mCurrentQueryRevision) {
    // The reasoner keeps data for every ontology it has processed (keyed by
    // the ontology pointer); release it before deleting the ontology, so no
    // entry is left pointing at freed memory.
    CReleaseOntologyCommand *releaseCommand =
        new CReleaseOntologyCommand(mCurrentQueryRevision->getOntology());
    mInstanceManager->getPreconditionSynchronizer()->delegateCommand(
        releaseCommand);
    CCommandExecutedBlocker::waitExecutedCommand(releaseCommand);
    delete releaseCommand;
  }
  delete mCurrentQueryRevision;
  mCurrentQueryRevision = nullptr;
}

bool CEmbeddedQueryManager::extractBooleanResult(
    CKnowledgeBaseQueryCommand *command, bool *resultOut) {
  if (!command || !resultOut) {
    mInstanceManager->setLastError(
        "Internal error: missing command or output pointer.");
    return false;
  }
  CCalculateQueryCommand *calcCommand = command->getCalculateQueryCommand();
  if (!calcCommand) {
    mInstanceManager->setLastError(
        "Query produced no calculate-query sub-command.");
    return false;
  }
  CQuery *query = calcCommand->getQuery();
  if (!query) {
    mInstanceManager->setLastError(
        "Query object missing from calculate-query command.");
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
  if (sparqlSelectQuery.trimmed().isEmpty()) {
    mInstanceManager->setLastError("Empty SPARQL query text.");
    return false;
  }

  // Query-specific entities are built into a never-installed revision
  // layered on top of the installed state (as COWLlinkProcessor's
  // SPARQL_QUERY branch does), so the state itself stays untouched. It is
  // created lazily and reused until the state changes (resetForNewState()).
  if (!mCurrentQueryRevision) {
    CCreateKnowledgeBaseRevisionUpdateCommand *updateCommand =
        new CCreateKnowledgeBaseRevisionUpdateCommand(
            mInstanceManager->getKnowledgeBaseName());
    mInstanceManager->getPreconditionSynchronizer()->delegateCommand(
        updateCommand);
    CCommandExecutedBlocker::waitExecutedCommand(updateCommand);
    mCurrentQueryRevision = updateCommand->getOntologyRevision();
    delete updateCommand;
    if (!mCurrentQueryRevision) {
      mInstanceManager->setLastError(
          "Failed to create the query-layer knowledge base revision.");
      return false;
    }
  }

  CConcreteOntology *onto = mCurrentQueryRevision->getOntology();
  CConcreteOntology *baseOnt =
      mCurrentQueryRevision->getPreviousOntologyRevision()->getOntology();
  COntologyConfigurationExtension *ontConfig =
      mCurrentQueryRevision->getOntologyConfiguration();

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
  mInstanceManager->getPreconditionSynchronizer()->delegateCommand(
      prepQueryCommand);
  CCommandExecutedBlocker::waitExecutedCommand(prepQueryCommand);
  delete prepQueryCommand;

  CCalculateQueryCommand *calcQueryCommand = new CCalculateQueryCommand(query);
  mInstanceManager->getPreconditionSynchronizer()->delegateCommand(
      calcQueryCommand);
  CCommandExecutedBlocker::waitExecutedCommand(calcQueryCommand);
  delete calcQueryCommand;

  // query itself (unlike mCurrentQueryRevision) is deleted below, once its
  // result has been copied out into plain Qt containers -- as
  // CJNIQueryProcessor's queryOntology*() methods do.
  CQueryResult *result = query->getQueryResult();
  CVariableBindingsAnswersResult *varBindAnsRes =
      dynamic_cast<CVariableBindingsAnswersResult *>(result);
  if (!varBindAnsRes) {
    mInstanceManager->setLastError(
        "Query did not produce a variable-bindings result.");
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
