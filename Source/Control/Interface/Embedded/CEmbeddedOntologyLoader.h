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

		namespace Generator {
			class CConcreteOntologyUpdateCollectorBuilder;
		}; // end namespace Generator

	}; // end namespace Reasoner

	namespace Control {

		namespace Interface {

			namespace Embedded {

				//! Qt-side copy of the C API's KoncludeClassAssertion.
				struct ClassAssertionFact {
					QString individualIRI;
					QString classIRI;
				};

				class CEmbeddedReasoner;

				/*!
				 *		\class		CEmbeddedOntologyLoader
				 *		\brief		Ontology loader/builder collaborator of CEmbeddedReasoner:
				 *					loads the base ontology once, then applies each FD state
				 *					transition as an incremental Tell/Retract on the single
				 *					knowledge base -- one new, installed revision per
				 *					tellAndRetractAssertions() call, exactly like an OWLlink
				 *					<Tell>/<Retract> request (COWLlinkProcessor.cpp, the
				 *					CTellKnowledgeBaseOWL2XMLNodeCommand branch). See
				 *					CEmbeddedReasoner for the shared instance infrastructure
				 *					and CEmbeddedQueryManager for querying the installed state.
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

						//! Moves the knowledge base to the next state: retracts
						//! retractAssertions, then tells tellAssertions, in one new
						//! revision, and installs it as the knowledge base's current
						//! revision (so every later query sees it). Retracting a fact that
						//! is not currently asserted is a silent no-op (builder semantics,
						//! CConcreteOntologyUpdateCollectorBuilder::retractOntologyAxiom()).
						//! A call with no tells and no retracts changes nothing. Installed
						//! revisions are retained by CSPOntologyRevisionManager and never
						//! freed (see docs/INCREMENTAL_REASONING.md §4).
						bool tellAndRetractAssertions(const ClassAssertionFact* tellAssertions, int tellAssertionCount, const ClassAssertionFact* retractAssertions, int retractAssertionCount);

						//! Diagnostic probe: repeatedly creates a scratch revision and
						//! deletes it again without installing it. Prints one progress line
						//! per iteration to stderr; returns the number of completed
						//! iterations (a hang never returns, so run under `timeout`).
						int probeScratchRevisionCycles(int iterations);

						bool isOntologyLoaded() const;

					// private methods
					private:
						bool addAssertionAxioms(Reasoner::Generator::CConcreteOntologyUpdateCollectorBuilder* builder, const ClassAssertionFact* assertions, int assertionCount, bool tell);

					// private variables
					private:
						CEmbeddedReasoner* mInstanceManager;

						bool mOntologyLoaded;
				};

			}; // end namespace Embedded

		}; // end namespace Interface

	}; // end namespace Control

}; // end namespace Konclude

#endif // KONCLUDE_COMPILE_EMBEDDED_INTERFACE

#endif // KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDONTOLOGYLOADER_H
