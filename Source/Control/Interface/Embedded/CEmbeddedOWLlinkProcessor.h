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

#ifndef KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDOWLLINKPROCESSOR_H
#define KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDOWLLINKPROCESSOR_H

#ifdef KONCLUDE_COMPILE_EMBEDDED_INTERFACE

// Namespace includes
#include "Control/Interface/OWLlink/COWLlinkProcessor.h"


namespace Konclude {

	namespace Control {

		namespace Interface {

			using namespace OWLlink;

			namespace Embedded {

				/*!
				 *		\class		CEmbeddedOWLlinkProcessor
				 *		\brief		Minimal COWLlinkProcessor subclass that gives
				 *					CEmbeddedReasoner a working command delegater for the
				 *					commands only ever handled inside
				 *					COWLlinkProcessor::processCustomsEvents (ontology loading,
				 *					consistency, satisfiability) -- CEmbeddedReasoner previously
				 *					never registered any COWLlinkProcessor-derived delegater at
				 *					all, so those commands were dispatched but never processed,
				 *					hanging CCommandExecutedBlocker::waitExecutedCommand()
				 *					forever. Modeled directly on the proven
				 *					Control/Interface/JNI/CJNICommandProcessor (the JNI
				 *					embedding interface's equivalent), stripped of the
				 *					JNI-bridge-specific helper methods this doesn't need.
				 */
				class CEmbeddedOWLlinkProcessor : public COWLlinkProcessor {

					// public methods
					public:
						//! Constructor -- starts its own worker thread only once
						//! reasonerCommander is populated from loaderConfig (mirrors
						//! CJNICommandProcessor's deferred-start pattern, avoiding a race
						//! where a posted command event could be processed before
						//! reasonerCommander is set).
						CEmbeddedOWLlinkProcessor(CConfiguration* loaderConfig);

						//! Destructor
						virtual ~CEmbeddedOWLlinkProcessor();

						virtual CConfiguration *getConfiguration();

					// protected methods
					protected:
						//! Only reachable via COWLlinkProcessor::startProcessing() or an
						//! explicit processed-callback registration -- CEmbeddedReasoner
						//! uses neither, so these are never actually invoked here; still
						//! required overrides since COWLlinkProcessor declares them pure
						//! virtual.
						virtual COWLlinkProcessor* initializeOWLlinkContent();
						virtual COWLlinkProcessor* concludeOWLlinkContent();

					// private variables
					private:
						CConfiguration* mLoaderConfig;

				};

			}; // end namespace Embedded

		}; // end namespace Interface

	}; // end namespace Control

}; // end namespace Konclude

#endif // KONCLUDE_COMPILE_EMBEDDED_INTERFACE

#endif // KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDOWLLINKPROCESSOR_H
