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

#ifndef KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDQUERYMANAGER_H
#define KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDQUERYMANAGER_H

#ifdef KONCLUDE_COMPILE_EMBEDDED_INTERFACE

// Libraries includes
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QList>


namespace Konclude {

	namespace Reasoner {

		namespace Revision {
			class COntologyRevision;
		}; // end namespace Revision

	}; // end namespace Reasoner

	namespace Control {

		namespace Command {

			namespace Instructions {
				class CKnowledgeBaseQueryCommand;
			}; // end namespace Instructions

		}; // end namespace Command

		namespace Interface {

			namespace Embedded {

				class CEmbeddedReasoner;
				class CEmbeddedOntologyLoader;

				/*!
				 *		\class		CEmbeddedQueryManager
				 *		\brief		Query manager collaborator of CEmbeddedReasoner: runs
				 *					every kind of query supported by the embedded interface
				 *					(consistency, class satisfiability, and single-BGP SPARQL
				 *					conjunctive queries against the current FD state built by
				 *					CEmbeddedOntologyLoader) and caches the most recent
				 *					conjunctive-query result for retrieval through the
				 *					konclude_embedded.h C API. See CEmbeddedReasoner for the
				 *					shared instance infrastructure (config, command delegation,
				 *					error reporting) this is constructed against.
				 *
				 *					Not safe to call concurrently from multiple threads; owned
				 *					exclusively by, and only ever accessed through, its
				 *					CEmbeddedReasoner instance.
				 */
				class CEmbeddedQueryManager {
					// public methods
					public:
						CEmbeddedQueryManager(CEmbeddedReasoner* instanceManager, CEmbeddedOntologyLoader* ontologyLoader);
						~CEmbeddedQueryManager();

						bool checkConsistency(bool* consistentOut);
						bool checkSatisfiability(const QString& classIRI, bool* satisfiableOut);

						//! Runs a single-BGP SPARQL SELECT conjunctive query against the
						//! ontology loader's current state (see CEmbeddedOntologyLoader::
						//! beginNewState()/assertClassFact() -- this reuses one query-layer
						//! revision across every consecutive query against the same state,
						//! so multiple queries against the same asserted ABox see each other
						//! and the asserted facts; see docs/CONJUNCTIVE_QUERY_PIPELINE.md for
						//! the query engine this uses -- the same non-Rasqal path as
						//! sparqlfile/sparqlserver, since Rasqal is never linked into
						//! KoncludeEmbedded.pro). Requires CEmbeddedOntologyLoader::
						//! beginNewState() to have been called first. Results are captured
						//! and retrievable via the getLastQueryResult* accessors below until
						//! the next call.
						bool executeConjunctiveQuery(const QString& sparqlSelectQuery);

						//! Drops the query-layer revision built for the previous state --
						//! called by CEmbeddedReasoner::beginNewState() alongside
						//! CEmbeddedOntologyLoader::beginNewState(). Safe to call even if no
						//! query has run yet (mCurrentQueryRevision is then already null).
						void resetForNewState();

						int getLastQueryResultRowCount();
						int getLastQueryResultVariableCount();
						//! UTF-8 pointer valid until the next call on this instance.
						const char* getLastQueryResultVariableNameCStr(int varIndex);
						//! UTF-8 pointer valid until the next call on this instance.
						const char* getLastQueryResultBindingCStr(int row, int varIndex);

					// private methods
					private:
						bool extractBooleanResult(Command::Instructions::CKnowledgeBaseQueryCommand* command, bool* resultOut);

					// private variables
					private:
						CEmbeddedReasoner* mInstanceManager;
						CEmbeddedOntologyLoader* mOntologyLoader;

						//! A revision layered on top of the ontology loader's current state
						//! revision, created lazily by the first executeConjunctiveQuery()
						//! call for this state and REUSED for every subsequent query against
						//! the same state (matches COWLlinkProcessor's own SPARQL_QUERY
						//! batching behaviour, which shares one such layer across
						//! consecutive queries too). Never installed; owned here; freed by
						//! resetForNewState() and the destructor.
						Reasoner::Revision::COntologyRevision* mCurrentQueryRevision;

						//! Result of the most recent executeConjunctiveQuery() call,
						//! extracted eagerly into plain Qt containers so the CQuery/
						//! CQueryResult object graph doesn't need to stay alive for the
						//! caller to read it back through the C API.
						QStringList mLastQueryVariableNames;
						QList<QStringList> mLastQueryResultRows;
						QByteArray mLastQueryResultStringUtf8;
				};

			}; // end namespace Embedded

		}; // end namespace Interface

	}; // end namespace Control

}; // end namespace Konclude

#endif // KONCLUDE_COMPILE_EMBEDDED_INTERFACE

#endif // KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDQUERYMANAGER_H
