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

#ifndef KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDONTOLOGYLOADER_H
#define KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDONTOLOGYLOADER_H

#ifdef KONCLUDE_COMPILE_EMBEDDED_INTERFACE

// Libraries includes
#include <QString>


namespace Konclude {

	namespace Reasoner {

		namespace Revision {
			class COntologyRevision;
		}; // end namespace Revision

		namespace Generator {
			class CConcreteOntologyUpdateCollectorBuilder;
		}; // end namespace Generator

	}; // end namespace Reasoner

	namespace Control {

		namespace Interface {

			namespace Embedded {

				class CEmbeddedReasoner;

				/*!
				 *		\class		CEmbeddedOntologyLoader
				 *		\brief		Ontology loader/builder collaborator of CEmbeddedReasoner:
				 *					loads the base ontology once, and owns the current FD
				 *					"state" -- a scratch ABox revision layered on top of the
				 *					loaded/classified base ontology, into which ground facts
				 *					are asserted one at a time. See CEmbeddedReasoner for the
				 *					shared instance infrastructure (config, command delegation,
				 *					error reporting) this is constructed against, and
				 *					CEmbeddedQueryManager for the collaborator that queries the
				 *					state this class builds.
				 *
				 *					Not safe to call concurrently from multiple threads; owned
				 *					exclusively by, and only ever accessed through, its
				 *					CEmbeddedReasoner instance.
				 */
				class CEmbeddedOntologyLoader {
					// public methods
					public:
						explicit CEmbeddedOntologyLoader(CEmbeddedReasoner* instanceManager);
						~CEmbeddedOntologyLoader();

						bool loadOntologyFile(const QString& filePath);

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

						bool isOntologyLoaded() const;
						bool hasCurrentState() const;

						//! Installs the current state's revision the first time this is
						//! called for it (idempotent thereafter), and returns it. Required
						//! before the state can be queried -- see CEmbeddedQueryManager::
						//! executeConjunctiveQuery() for why this is NOT optional (forcing
						//! query processing directly on a never-installed revision looked
						//! plausible but empirically corrupted query results, including for
						//! content that predates any Tell into this state).
						Reasoner::Revision::COntologyRevision* ensureCurrentStateInstalled();

					// private variables
					private:
						CEmbeddedReasoner* mInstanceManager;

						bool mOntologyLoaded;

						//! The current FD state's ABox revision. Starts out never-installed
						//! but gets installed lazily, once, by the first
						//! ensureCurrentStateInstalled() call for this state -- see
						//! mCurrentStateInstalled below for why. Owned here; freed
						//! explicitly by beginNewState() (on the next state) and the
						//! destructor UNLESS installed, in which case
						//! CSPOntologyRevisionManager's onRevContainer owns it instead. Null
						//! until the first beginNewState() call.
						Reasoner::Revision::COntologyRevision* mCurrentStateRevision;
						//! Whether mCurrentStateRevision has been installed yet. Reset to
						//! false by beginNewState(). Installing turns out to be required,
						//! not optional, for facts asserted into mCurrentStateRevision to be
						//! visible to any query at all. Every existing Tell path in this
						//! codebase (COWLlinkProcessor.cpp's SPARQL UPDATE MODIFY handling)
						//! installs before any query ever touches the Told revision;
						//! nothing in the codebase does otherwise.
						bool mCurrentStateInstalled;

						//! One CConcreteOntologyUpdateCollectorBuilder per current state,
						//! tied to mCurrentStateRevision's ontology and reused across every
						//! assertClassFact() call for that state instead of allocating a
						//! fresh builder per fact -- safe because initializeBuilding()/
						//! completeBuilding() fully re-sync the builder's working sets
						//! (including the entity/axiom-number counters) from/to the shared
						//! ontology's build data on every call, so no state leaks between
						//! successive assert calls regardless of builder object identity.
						//! Rebuilt by beginNewState() (whose new state has a different
						//! ontology) and freed by the destructor.
						Reasoner::Generator::CConcreteOntologyUpdateCollectorBuilder* mCurrentStateBuilder;
				};

			}; // end namespace Embedded

		}; // end namespace Interface

	}; // end namespace Control

}; // end namespace Konclude

#endif // KONCLUDE_COMPILE_EMBEDDED_INTERFACE

#endif // KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDONTOLOGYLOADER_H
