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

// Logger includes
#include "Logger/CLogger.h"
#include "Logger/CAbstractLogObserver.h"
#include "Logger/CLogMessage.h"

using namespace Konclude::Config;
using namespace Konclude::Logger;
using namespace Konclude::Control::Command;
using namespace Konclude::Control::Command::Instructions;


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
						bool mOntologyLoaded;

						QString mLastError;
						QByteArray mLastErrorUtf8;
				};

			}; // end namespace Embedded

		}; // end namespace Interface

	}; // end namespace Control

}; // end namespace Konclude

#endif // KONCLUDE_COMPILE_EMBEDDED_INTERFACE

#endif // KONCLUDE_CONTROL_INTERFACE_EMBEDDED_CEMBEDDEDREASONER_H
