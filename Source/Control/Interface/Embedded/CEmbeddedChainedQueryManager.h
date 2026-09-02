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

#ifndef KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDCHAINEDQUERYMANAGER_H
#define KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDCHAINEDQUERYMANAGER_H

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

		namespace Interface {

			namespace Embedded {

				class CEmbeddedReasoner;
				class CEmbeddedChainedOntologyLoader;

				/*!
				 *		\class		CEmbeddedChainedQueryManager
				 *		\brief		Query counterpart to CEmbeddedChainedOntologyLoader --
				 *					together they are the correctness-experiment sibling of
				 *					CEmbeddedQueryManager/CEmbeddedOntologyLoader, kept in
				 *					separate files/classes so the experiment never touches
				 *					the production query path either. Otherwise identical in
				 *					design to CEmbeddedQueryManager::executeConjunctiveQuery():
				 *					builds and reuses one query-layer revision on top of the
				 *					chained loader's current (installed) head across every
				 *					consecutive query against the same chained state, until
				 *					resetForNewState() is called. See
				 *					docs/EMBEDDED_STATE_ISOLATION_BUG.md's "Confirmed
				 *					empirically" entry for what this pairing tests.
				 *
				 *					Not safe to call concurrently from multiple threads;
				 *					owned exclusively by, and only ever accessed through, its
				 *					CEmbeddedReasoner instance.
				 */
				class CEmbeddedChainedQueryManager {
					// public methods
					public:
						CEmbeddedChainedQueryManager(CEmbeddedReasoner* instanceManager, CEmbeddedChainedOntologyLoader* chainedOntologyLoader);
						~CEmbeddedChainedQueryManager();

						//! See CEmbeddedQueryManager::executeConjunctiveQuery() -- identical
						//! mechanism, run against CEmbeddedChainedOntologyLoader's current
						//! chained state instead. Requires
						//! CEmbeddedChainedOntologyLoader::beginNewChainedState() to have
						//! been called first.
						bool executeConjunctiveQuery(const QString& sparqlSelectQuery);

						//! Drops the query-layer revision built for the previous chained
						//! state/Tell -- call after CEmbeddedChainedOntologyLoader::
						//! beginNewChainedState() and after every successful
						//! assertClassFactChained()/retractClassFactChained() (mirrors
						//! COWLlinkProcessor's lastGetCurrKBRevC reset on any SPARQL update
						//! operation). Safe to call even if no query has run yet.
						void resetForNewState();

						int getLastQueryResultRowCount();
						int getLastQueryResultVariableCount();
						//! UTF-8 pointer valid until the next call on this instance.
						const char* getLastQueryResultVariableNameCStr(int varIndex);
						//! UTF-8 pointer valid until the next call on this instance.
						const char* getLastQueryResultBindingCStr(int row, int varIndex);

					// private variables
					private:
						CEmbeddedReasoner* mInstanceManager;
						CEmbeddedChainedOntologyLoader* mChainedOntologyLoader;

						//! See CEmbeddedQueryManager::mCurrentQueryRevision's doc comment --
						//! identical role, layered on the chained loader's current head
						//! instead. Never installed; owned here; freed by
						//! resetForNewState() and the destructor.
						Reasoner::Revision::COntologyRevision* mCurrentQueryRevision;

						QStringList mLastQueryVariableNames;
						QList<QStringList> mLastQueryResultRows;
						QByteArray mLastQueryResultStringUtf8;
				};

			}; // end namespace Embedded

		}; // end namespace Interface

	}; // end namespace Control

}; // end namespace Konclude

#endif // KONCLUDE_COMPILE_EMBEDDED_INTERFACE

#endif // KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDCHAINEDQUERYMANAGER_H
