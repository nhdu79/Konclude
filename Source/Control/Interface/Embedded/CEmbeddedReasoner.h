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
#include "Control/Command/Instructions/CKnowledgeBaseQueryCommand.h"
#include "Control/Interface/OWLlink/COWLlinkProcessor.h"
#include "Reasoner/Revision/COntologyRevision.h"

// Logger includes
#include "Logger/CLogger.h"
#include "Logger/CAbstractLogObserver.h"
#include "Logger/CLogMessage.h"

using namespace Konclude::Config;
using namespace Konclude::Logger;
using namespace Konclude::Control::Command;
using namespace Konclude::Control::Command::Instructions;
using namespace Konclude::Control::Interface::OWLlink;
using namespace Konclude::Reasoner::Revision;


namespace Konclude {

	namespace Control {

		namespace Interface {

			namespace Embedded {

				/*!
				 *		\class		CEmbeddedReasoner
				 *		\brief		Wraps a single, independent CCommanderManager instance
				 *					(own worker threads, own knowledge base namespace) behind
				 *					a synchronous, blocking call interface, for use behind the
				 *					konclude_embedded.h C API. See docs/FASTDOWNWARD_EMBEDDING.md
				 *					for the design this mirrors from the JNI/CLI code paths.
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

						//! Starts a new FD "state": discards whatever scratch ABox revision
						//! was current (see docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md's
						//! Decision 2 -- never installed, so a plain delete is the whole
						//! cleanup) and creates a fresh one layered on the loaded/classified
						//! base ontology via CCreateKnowledgeBaseRevisionUpdateCommand. Call
						//! this once per FD state, before asserting that state's ABox facts.
						bool beginNewState();

						//! Asserts one ClassAssertion(individualIRI, classIRI) ground fact
						//! into the current state's scratch revision (see beginNewState()),
						//! via the same CConcreteOntologyUpdateCollectorBuilder/
						//! tellOntologyAxiom mechanism every file-based ontology loader in
						//! this codebase uses -- not a preset CConcreteOntology handed in
						//! from outside (see CCreateKnowledgeBaseRevisionUpdateCommand's
						//! ABox/query constructor -- unused anywhere in this codebase and,
						//! per createNewOntologyRevision's presetOntology handling, untested).
						//! Requires beginNewState() to have been called first.
						bool assertClassFact(const QString& individualIRI, const QString& classIRI);

						//! Runs a single-BGP SPARQL SELECT conjunctive query against the
						//! current state's scratch revision (see beginNewState()/
						//! assertClassFact() above -- this no longer creates its own
						//! revision per call, so multiple queries against the same asserted
						//! ABox now actually see each other and the asserted facts; see
						//! docs/CONJUNCTIVE_QUERY_PIPELINE.md for the query engine this
						//! uses -- the same non-Rasqal path as sparqlfile/sparqlserver,
						//! since Rasqal is never linked into KoncludeEmbedded.pro). Requires
						//! beginNewState() to have been called first. Results are captured
						//! and retrievable via the getLastQueryResult* accessors below until
						//! the next call.
						bool executeConjunctiveQuery(const QString& sparqlSelectQuery);

						//! Correctness probe for docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md's
						//! Decision 4 open item: repeatedly builds a scratch knowledge-base
						//! revision via CCreateKnowledgeBaseRevisionUpdateCommand -- WITHOUT
						//! ever installing it -- and immediately deletes it again, asserting
						//! no facts. Prints one progress line per iteration to stderr, so
						//! that running this under a shell-level timeout (e.g. `timeout 30`)
						//! still shows how many iterations completed before a hang, if the
						//! CPreconditionSynchronizer dispatch bug documented in
						//! docs/FASTDOWNWARD_EMBEDDING.md reproduces for this exact command
						//! sequence. Returns the number of iterations that completed; a hang
						//! never returns at all, by definition, so a short return count with
						//! no crash/error message means "it hung," not "it failed".
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

					// private methods
					private:
						static void ensureQCoreApplication();

						bool extractBooleanResult(CKnowledgeBaseQueryCommand* command, bool* resultOut);

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
						bool mOntologyLoaded;

						//! The current FD state's ABox revision. Starts out never-installed
						//! (see docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md's Decision 2)
						//! but gets installed lazily, once, by the first executeConjunctiveQuery()
						//! call for this state -- see mCurrentStateInstalled below for why.
						//! Owned here; freed explicitly by beginNewState() (on the next
						//! state) and the destructor UNLESS installed, in which case
						//! CSPOntologyRevisionManager's onRevContainer owns it instead (see
						//! CEmbeddedReasoner.cpp). Null until the first beginNewState() call.
						COntologyRevision* mCurrentStateRevision;
						//! Whether mCurrentStateRevision has been installed yet. Reset to
						//! false by beginNewState(). Installing turns out to be required,
						//! not optional, for facts asserted into mCurrentStateRevision to be
						//! visible to any query at all -- see the long comment on
						//! executeConjunctiveQuery()'s implementation for what was tried and
						//! ruled out first (forcing OPSBUILD directly on a never-installed
						//! revision looked plausible but empirically corrupted query results,
						//! including for content that predates the Tell). Every existing Tell
						//! path in this codebase (COWLlinkProcessor.cpp's SPARQL UPDATE
						//! MODIFY handling) installs before any query ever touches the
						//! Told revision; nothing in the codebase does otherwise.
						bool mCurrentStateInstalled;

						//! A revision layered on top of mCurrentStateRevision, created
						//! lazily by the first executeConjunctiveQuery() call for this
						//! state and REUSED for every subsequent query against the same
						//! state (matches COWLlinkProcessor's own SPARQL_QUERY batching
						//! behaviour, which shares one such layer across consecutive
						//! queries too -- see the long comment on executeConjunctiveQuery()'s
						//! implementation). Never installed; owned here; freed by
						//! beginNewState() (on the next state) and the destructor.
						COntologyRevision* mCurrentQueryRevision;

						QString mLastError;
						QByteArray mLastErrorUtf8;

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

#endif // KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDREASONER_H
