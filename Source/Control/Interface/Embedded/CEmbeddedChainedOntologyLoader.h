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

#ifndef KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDCHAINEDONTOLOGYLOADER_H
#define KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDCHAINEDONTOLOGYLOADER_H

#ifdef KONCLUDE_COMPILE_EMBEDDED_INTERFACE

// Libraries includes
#include <QString>


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

				/*!
				 *		\class		CEmbeddedChainedOntologyLoader
				 *		\brief		Correctness-experiment counterpart to
				 *					CEmbeddedOntologyLoader, deliberately kept in its own
				 *					class/file pair so this experiment never touches the
				 *					production Tell path (CEmbeddedOntologyLoader::
				 *					assertClassFact(), CEmbeddedOntologyLoader::
				 *					beginNewState()) or shares any state with it. See
				 *					docs/EMBEDDED_STATE_ISOLATION_BUG.md's "Confirmed
				 *					empirically" entry under "Attempted fixes (ruled out)"
				 *					for what this tests and why.
				 *
				 *					Unlike CEmbeddedOntologyLoader::assertClassFact() (which
				 *					accumulates every Tell into ONE shared, lazily-installed
				 *					state revision -- see its own doc comment), every
				 *					assertClassFactChained()/retractClassFactChained() call
				 *					here creates a BRAND NEW revision layered on the current
				 *					installed head, Tells/retracts the single fact into it,
				 *					and installs it immediately -- mirroring
				 *					COWLlinkProcessor::processCustomsEvents'
				 *					SPARQL_UPDATE_MODIFY handling (~line 1133-1150)
				 *					call-for-call: Create -> Tell/Retract -> Install,
				 *					extending a genuine chain of installed revisions (each
				 *					with its own fresh ontologyID) rather than repeatedly
				 *					Telling into one static, once-installed base.
				 *
				 *					NOT wired into any production path: installing on every
				 *					single fact reintroduces the per-call install cost
				 *					docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md's Decision 2
				 *					explicitly rejected (O(N) referenceBuildData() copy per
				 *					call instead of per state) -- correctness experiment
				 *					only, driven by
				 *					Tools/EmbeddedDriver/embedded_state_isolation_chained_probe.cpp
				 *					via the *_chained C API functions in konclude_embedded.h.
				 *
				 *					Not safe to call concurrently from multiple threads;
				 *					owned exclusively by, and only ever accessed through, its
				 *					CEmbeddedReasoner instance. Requires an ontology already
				 *					loaded via CEmbeddedReasoner::loadOntologyFile() (through
				 *					the production CEmbeddedOntologyLoader, which every
				 *					CEmbeddedReasoner instance already constructs) before
				 *					beginNewChainedState() is called.
				 */
				class CEmbeddedChainedOntologyLoader {
					// public methods
					public:
						explicit CEmbeddedChainedOntologyLoader(CEmbeddedReasoner* instanceManager);
						~CEmbeddedChainedOntologyLoader();

						//! Starts a new chained "state": drops whatever chained revision
						//! was current (a plain delete if it was never installed -- i.e.
						//! no chained Tell/retract ever ran for it -- or just dropping the
						//! pointer if it was, since CSPOntologyRevisionManager's
						//! onRevContainer owns it once installed) and creates a fresh,
						//! NOT-yet-installed revision layered on the knowledge base's
						//! current installed revision. Call once per chained "state",
						//! before assertClassFactChained()/retractClassFactChained().
						bool beginNewChainedState();

						//! See the class doc comment above for what makes this different
						//! from CEmbeddedOntologyLoader::assertClassFact(). Requires
						//! beginNewChainedState() to have been called first.
						bool assertClassFactChained(const QString& individualIRI, const QString& classIRI);
						bool retractClassFactChained(const QString& individualIRI, const QString& classIRI);

						bool hasCurrentState() const;

						//! Installs the chain's current head if it isn't already
						//! (idempotent) -- needed so a query can run even if no chained
						//! Tell/retract has happened yet for this state. Mirrors
						//! CEmbeddedOntologyLoader::ensureCurrentStateInstalled().
						Reasoner::Revision::COntologyRevision* ensureCurrentRevisionInstalled();

					// private methods
					private:
						//! Shared implementation of assertClassFactChained()/
						//! retractClassFactChained() -- see their doc comments above.
						bool tellOrRetractClassFactChained(const QString& individualIRI, const QString& classIRI, bool retract);

					// private variables
					private:
						CEmbeddedReasoner* mInstanceManager;

						//! The chain's current head. Owned here UNLESS installed, in which
						//! case CSPOntologyRevisionManager's onRevContainer owns it
						//! instead (see the destructor and beginNewChainedState()). Null
						//! until the first beginNewChainedState() call.
						Reasoner::Revision::COntologyRevision* mCurrentRevision;
						//! Whether mCurrentRevision has been installed yet. Reset to false
						//! by beginNewChainedState(); every successful chained Tell/
						//! retract sets it true again immediately (unlike
						//! CEmbeddedOntologyLoader's equivalent flag, which stays false
						//! until the state's first query).
						bool mCurrentRevisionInstalled;
				};

			}; // end namespace Embedded

		}; // end namespace Interface

	}; // end namespace Control

}; // end namespace Konclude

#endif // KONCLUDE_COMPILE_EMBEDDED_INTERFACE

#endif // KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDCHAINEDONTOLOGYLOADER_H
