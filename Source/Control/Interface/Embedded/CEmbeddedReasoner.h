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

#ifndef KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDREASONER_H
#define KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDREASONER_H

#ifdef KONCLUDE_COMPILE_EMBEDDED_INTERFACE

// Libraries includes
#include <QString>
#include <QByteArray>

// Namespace includes
#include "Config/CConfiguration.h"
#include "Control/Command/CCommanderManager.h"
#include "Control/Command/CPreconditionSynchronizer.h"
#include "Control/Interface/OWLlink/COWLlinkProcessor.h"

// Logger includes
#include "Logger/CLogger.h"
#include "Logger/CAbstractLogObserver.h"
#include "Logger/CLogMessage.h"

using namespace Konclude::Config;
using namespace Konclude::Logger;
using namespace Konclude::Control::Command;
using namespace Konclude::Control::Interface::OWLlink;


namespace Konclude {

	namespace Control {

		namespace Interface {

			namespace Embedded {

				class CEmbeddedOntologyLoader;
				class CEmbeddedQueryManager;

				/*!
				 *		\class		CEmbeddedReasoner
				 *		\brief		Central instance manager: wraps a single, independent
				 *					CCommanderManager instance (own worker threads, own
				 *					knowledge base namespace) behind a synchronous, blocking
				 *					call interface, for use behind the konclude_embedded.h C
				 *					API. See docs/FASTDOWNWARD_EMBEDDING.md for the design this
				 *					mirrors from the JNI/CLI code paths.
				 *
				 *					Owns the shared instance infrastructure -- configuration,
				 *					command delegation (CPreconditionSynchronizer), the OWLlink
				 *					command delegater, and error/log-message capture -- and
				 *					delegates every reasoning operation to two collaborators
				 *					constructed against it: CEmbeddedOntologyLoader (ontology
				 *					loading and FD-state/ABox building) and
				 *					CEmbeddedQueryManager (consistency/satisfiability/
				 *					conjunctive-query execution). Public method signatures are
				 *					unchanged from before that split -- this class remains the
				 *					sole type the konclude_embedded.h C API shim
				 *					(CEmbeddedInterfaceCAPI.cpp) talks to.
				 *
				 *					Not safe to call concurrently from multiple threads on the
				 *					same instance; separate instances are independent.
				 */
				class CEmbeddedReasoner : public CAbstractLogObserver {
					// public methods
					public:
						CEmbeddedReasoner();
						virtual ~CEmbeddedReasoner();

						bool loadOntologyFile(const QString& filePath);
						bool checkConsistency(bool* consistentOut);
						bool checkSatisfiability(const QString& classIRI, bool* satisfiableOut);

						//! Starts a new FD "state" -- see CEmbeddedOntologyLoader::
						//! beginNewState() for the ontology-loader-side details, and
						//! CEmbeddedQueryManager::resetForNewState() for the query-manager
						//! side. Call this once per FD state, before asserting that state's
						//! ABox facts.
						bool beginNewState();

						//! Asserts one ClassAssertion(individualIRI, classIRI) ground fact
						//! into the current state -- see CEmbeddedOntologyLoader::
						//! assertClassFact(). Requires beginNewState() to have been called
						//! first.
						bool assertClassFact(const QString& individualIRI, const QString& classIRI);

						//! Runs a single-BGP SPARQL SELECT conjunctive query against the
						//! current state -- see CEmbeddedQueryManager::
						//! executeConjunctiveQuery(). Requires beginNewState() to have been
						//! called first. Results are captured and retrievable via the
						//! getLastQueryResult* accessors below until the next call.
						bool executeConjunctiveQuery(const QString& sparqlSelectQuery);

						//! Correctness probe -- see CEmbeddedOntologyLoader::
						//! probeScratchRevisionCycles(). Returns the number of iterations
						//! that completed; a hang never returns at all, by definition, so a
						//! short return count with no crash/error message means "it hung,"
						//! not "it failed".
						int probeScratchRevisionCycles(int iterations);

						int getLastQueryResultRowCount();
						int getLastQueryResultVariableCount();
						//! UTF-8 pointer valid until the next call on this instance.
						const char* getLastQueryResultVariableNameCStr(int varIndex);
						//! UTF-8 pointer valid until the next call on this instance.
						const char* getLastQueryResultBindingCStr(int row, int varIndex);

						//! UTF-8 pointer valid until the next call on this instance.
						const char* getLastErrorCStr();

						// CAbstractLogObserver
						virtual void postLogMessage(CLogMessage* message);

						// --- Collaborator-facing services -----------------------------
						// The methods below are the interface CEmbeddedOntologyLoader and
						// CEmbeddedQueryManager use to reach this instance's shared
						// infrastructure; not intended for the konclude_embedded.h C API
						// surface.

						//! Whether construction succeeded and commands can be delegated.
						//! False means every public method above will fail, with
						//! getLastErrorCStr() explaining why (set in the constructor).
						bool isReady() const;
						CPreconditionSynchronizer* getPreconditionSynchronizer() const;
						COWLlinkProcessor* getOwlLinkProcessor() const;
						const QString& getKnowledgeBaseName() const;
						void setLastError(const QString& error);

					// private methods
					private:
						static void ensureQCoreApplication();

					// private variables
					private:
						QString mKnowledgeBaseName;
						CConfiguration* mConfig;
						CCommanderManager* mReasonerCommander;
						//! Commands must be delegated through this, not mReasonerCommander
						//! directly -- it is what actually waits for/retries commands whose
						//! preconditions (e.g. reasoner initialization) aren't satisfied yet.
						//! Mirrors COWLlinkProcessor's preSynchronizer (see .cpp for detail).
						CPreconditionSynchronizer* mPreconditionSynchronizer;
						//! Handles the commands COWLlinkProcessor::processCustomsEvents is
						//! the only registered handler for (ontology loading, consistency,
						//! satisfiability) -- CCreateKnowledgeBaseCommand and the CQ path
						//! stay on mPreconditionSynchronizer/mReasonerCommander directly,
						//! which already handle those correctly. Non-null exactly when
						//! mReasonerCommander is (constructed together in the constructor's
						//! commanderConfigType branch).
						COWLlinkProcessor* mOwlLinkProcessor;

						//! Ontology loading and FD-state/ABox-building collaborator. Always
						//! constructed (even if the infrastructure above failed to set up --
						//! see isReady()); see CEmbeddedOntologyLoader.
						CEmbeddedOntologyLoader* mOntologyLoader;
						//! Query-execution collaborator. Always constructed (even if the
						//! infrastructure above failed to set up -- see isReady()); see
						//! CEmbeddedQueryManager.
						CEmbeddedQueryManager* mQueryManager;

						QString mLastError;
						QByteArray mLastErrorUtf8;
				};

			}; // end namespace Embedded

		}; // end namespace Interface

	}; // end namespace Control

}; // end namespace Konclude

#endif // KONCLUDE_COMPILE_EMBEDDED_INTERFACE

#endif // KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDREASONER_H
