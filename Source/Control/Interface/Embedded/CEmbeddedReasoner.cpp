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

#ifdef KONCLUDE_COMPILE_EMBEDDED_INTERFACE

#include "CEmbeddedReasoner.h"

#include <QCoreApplication>
#include <QFileInfo>

#include "Utilities/CSingletonProvider.hpp"

#include "Control/Command/CConfigManagerReader.h"
#include "Control/Command/CReasonerConfigurationGroup.h"
#include "Control/Command/CCommandExecutedBlocker.h"
#include "Control/Command/CCommanderManagerThread.h"
#include "Control/Command/CCommanderManagerConfigType.h"
#include "Control/Command/CDefaultCommanderInitializationFactory.h"

#include "Control/Command/Instructions/CCreateKnowledgeBaseCommand.h"
#include "Control/Command/Instructions/CLoadKnowledgeBaseOWLAutoOntologyCommand.h"
#include "Control/Command/Instructions/CIsConsistentQueryCommand.h"
#include "Control/Command/Instructions/CProcessClassNameSatisfiableQueryCommand.h"
#include "Control/Command/Instructions/CCalculateQueryCommand.h"
#include "Control/Command/Instructions/CInitializeConfigurationCommand.h"
#include "Control/Command/Instructions/CInitializeReasonerCommand.h"

#include "Reasoner/Query/CQuery.h"
#include "Reasoner/Query/CQueryResult.h"
#include "Reasoner/Query/CBooleanQueryResult.h"

using namespace Konclude::Utilities;
using namespace Konclude::Reasoner::Query;


namespace Konclude {

	namespace Control {

		namespace Interface {

			namespace Embedded {


				namespace {
					// The QCoreApplication instance is process-global (Konclude requires
					// exactly one per process) -- guard it so repeated CEmbeddedReasoner
					// create/destroy cycles within one process don't try to construct a
					// second one. See docs/FASTDOWNWARD_EMBEDDING.md phase 3.
					QCoreApplication* gEmbeddedQtApp = nullptr;

					// The known startup deadlock is triggered by exactly 1 processing
					// thread; never let this drop to 1 regardless of caller/environment.
					// NOTE: "Konclude.Calculation.ProcessorCount" is registered by
					// CReasonerConfigurationGroup's own constructor with a *string*
					// default of "1" (Control/Command/CReasonerConfigurationGroup.cpp) --
					// not an integer, and re-registering the name via addConfigProperty()
					// (tried first) silently created a second, differently-typed,
					// unread description rather than overriding the real one, reproducing
					// the exact single-thread deadlock this is meant to prevent. The
					// correct mechanism is a per-CConfiguration override via
					// createAndSetConfig()+readFromString(), mirroring exactly what the
					// CLI's "-w N" flag does (CCommandLineLoader::setConfiguration()) --
					// applied per-instance in the constructor below, not once globally.
					const char* cMinimumProcessorCountString = "2";
				}


				void CEmbeddedReasoner::ensureQCoreApplication() {
					if (QCoreApplication::instance() == nullptr) {
						static int argc = 1;
						static char appName[] = "Konclude";
						static char* argv[] = { appName, nullptr };
						// Deliberately never call exec() -- see docs/FASTDOWNWARD_EMBEDDING.md
						// phase 3: the JNI interface already proves Konclude's synchronous
						// command completion (CBlockingCallbackData / CCommandExecutedBlocker)
						// does not depend on a running Qt event loop.
						gEmbeddedQtApp = new QCoreApplication(argc, argv);
					}
				}


				CEmbeddedReasoner::CEmbeddedReasoner() : mConfig(nullptr), mReasonerCommander(nullptr), mPreconditionSynchronizer(nullptr), mOntologyLoaded(false) {
					ensureQCoreApplication();

					CLogger::getInstance()->addLogObserver(this, 70.0);

					CConfigurationGroup* reasonerConfigGroup = CSingletonProvider<CReasonerConfigurationGroup>::getInstance()->getReferencedConfigurationGroup();
					CConfiguration* rootConfig = new CConfiguration(reasonerConfigGroup);
					mConfig = new CConfiguration(rootConfig);

					// Each instance gets its own CCommanderManager -- own worker threads,
					// own knowledge-base namespace -- so a fixed KB name per instance is
					// fine; there is no cross-instance sharing to collide with.
					mKnowledgeBaseName = QString("http://konclude.com/embedded/kb");

					// Force >=2 processing threads on THIS instance's config, as a
					// per-CConfiguration override -- exactly mirroring what "-w N" does
					// for the CLI (CCommandLineLoader::setConfiguration(), which calls
					// createAndSetConfig()+readFromString() rather than touching the
					// shared group's description). Must happen before
					// CInitializeReasonerCommand below, which is what actually reads it.
					CConfigData* processorCountData = mConfig->createAndSetConfig("Konclude.Calculation.ProcessorCount");
					if (processorCountData) {
						processorCountData->readFromString(cMinimumProcessorCountString);
					}

					// CReasonerConfigurationGroup registers "Konclude.Execution.CommanderManager"
					// with a NULL default (CCommanderManagerConfigType(0)) -- on its own,
					// readCommanderManagerConfig() legitimately returns nullptr. The CLI's
					// "-DefaultReasonerLoader" (Control/Loader/CDefaultReasonerLoader.cpp)
					// is what actually constructs a CCommanderManagerThread and wires it in;
					// it is a CLoader we don't otherwise need, so its ~6-line init()/load()
					// sequence is replicated directly here instead of pulling in the
					// CLoader/CLoaderFactory machinery around it.
					//
					// NOTE (multi-instance caveat): CDefaultReasonerLoader writes the
					// manager via group->setConfigDefaultData(...) -- the GROUP's shared
					// default slot, not a per-CConfiguration override (it uses
					// createConfig(), not createAndSetConfig()). That slot is shared
					// across the whole process (CSingletonProvider<CReasonerConfigurationGroup>).
					// Sequential create -> use -> destroy of one CEmbeddedReasoner at a
					// time is fine (this constructor captures its own manager pointer
					// into mReasonerCommander immediately, used via direct method calls
					// afterward, not re-resolved through config). Genuinely concurrent,
					// simultaneously-alive instances are NOT proven safe by this
					// mechanism and need verification before being relied on -- flagged
					// for the phase 7 stress test, and the public header's claim about
					// independent handles has been narrowed until that's verified.
					CCommanderManagerThread* commanderThread = new CCommanderManagerThread();
					CConfigData* commanderConfigData = mConfig->createConfig("Konclude.Execution.CommanderManager");
					CCommanderManagerConfigType* commanderConfigType = commanderConfigData ? dynamic_cast<CCommanderManagerConfigType*>(commanderConfigData->getConfigType()) : nullptr;
					if (commanderConfigType) {
						commanderConfigType->setCommanderManager(commanderThread);
						reasonerConfigGroup->setConfigDefaultData(reasonerConfigGroup->getConfigIndex("Konclude.Execution.CommanderManager"), commanderConfigData);

						commanderThread->realizeCommand(new CInitializeConfigurationCommand(mConfig));
						commanderThread->realizeCommand(new CInitializeReasonerCommand(new CDefaultCommanderInitializationFactory()));

						mReasonerCommander = commanderThread;

						// Commands must be delegated through a CPreconditionSynchronizer,
						// not straight to mReasonerCommander -- discovered by reproducing a
						// hang: a CCreateKnowledgeBaseCommand delegated directly to the
						// commander sat forever because nothing was watching for its
						// preconditions (reasoner initialization, itself still in flight via
						// the two realizeCommand() calls above) to become satisfied. Every
						// COWLlinkProcessor-derived class in the codebase routes through
						// exactly this wrapper (its own preSynchronizer member) instead of
						// its underlying delegater directly -- same fix, applied here.
						mPreconditionSynchronizer = new CPreconditionSynchronizer(mReasonerCommander);
					} else {
						mLastError = "Failed to construct CCommanderManager: unexpected config type.";
						delete commanderThread;
					}
				}


				CEmbeddedReasoner::~CEmbeddedReasoner() {
					CLogger::getInstance()->removeObserverFromAllDomains(this);

					// NOTE: there is no proven precedent anywhere in Konclude's existing
					// code paths for tearing down a CCommanderManager while the process
					// keeps running -- the CLI/OWLlink loaders only ever run once and let
					// process exit reclaim everything. This relies on CCommanderManager's
					// virtual destructor alone. Flagged for the phase 7 stress test
					// (repeated create/use/destroy in a loop) rather than assumed safe.
					delete mPreconditionSynchronizer;
					delete mReasonerCommander;
					delete mConfig;
				}


				void CEmbeddedReasoner::postLogMessage(CLogMessage* message) {
					if (message) {
						mLastError = message->getMessage();
					}
				}


				const char* CEmbeddedReasoner::getLastErrorCStr() {
					mLastErrorUtf8 = mLastError.toUtf8();
					return mLastErrorUtf8.constData();
				}


				bool CEmbeddedReasoner::extractBooleanResult(CKnowledgeBaseQueryCommand* command, bool* resultOut) {
					if (!command || !resultOut) {
						mLastError = "Internal error: missing command or output pointer.";
						return false;
					}
					CCalculateQueryCommand* calcCommand = command->getCalculateQueryCommand();
					if (!calcCommand) {
						mLastError = "Query produced no calculate-query sub-command.";
						return false;
					}
					CQuery* query = calcCommand->getQuery();
					if (!query) {
						mLastError = "Query object missing from calculate-query command.";
						return false;
					}
					CQueryResult* result = query->getQueryResult();
					CBooleanQueryResult* boolResult = dynamic_cast<CBooleanQueryResult*>(result);
					if (!boolResult) {
						mLastError = "Query did not produce a boolean result.";
						return false;
					}
					*resultOut = boolResult->getResult();
					return true;
				}


				bool CEmbeddedReasoner::loadOntologyFile(const QString& filePath) {
					if (!mReasonerCommander) {
						// mLastError already set in the constructor if this happened.
						return false;
					}
					if (mOntologyLoaded) {
						mLastError = "Ontology already loaded for this reasoner instance; create a new instance to load a different ontology.";
						return false;
					}
					QFileInfo fileInfo(filePath);
					if (!fileInfo.exists()) {
						mLastError = QString("Ontology file does not exist: %1").arg(filePath);
						return false;
					}

					// Mirrors COREBatchProcessingLoader's proven create-then-load command
					// sequence (Source/Control/Loader/COREBatchProcessingLoader.cpp), the
					// same one exercised by the already-verified CLI classification/
					// satisfiability runs -- not hand-derived from the lower-level
					// IRI-resolver machinery.
					CCreateKnowledgeBaseCommand* createKBCommand = new CCreateKnowledgeBaseCommand(mKnowledgeBaseName);
					mPreconditionSynchronizer->delegateCommand(createKBCommand);
					CCommandExecutedBlocker::waitExecutedCommand(createKBCommand);

					QStringList ontoIRIList;
					ontoIRIList.append(filePath);
					CLoadKnowledgeBaseOWLAutoOntologyCommand* loadKBCommand = new CLoadKnowledgeBaseOWLAutoOntologyCommand(mKnowledgeBaseName, ontoIRIList);
					mPreconditionSynchronizer->delegateCommand(loadKBCommand);
					CCommandExecutedBlocker::waitExecutedCommand(loadKBCommand);

					// No structured success/failure signal exists on these commands (see
					// docs/FASTDOWNWARD_EMBEDDING.md phase 2) -- failures surface only via
					// the ERROR-level log messages captured by postLogMessage() above.
					mOntologyLoaded = true;
					return true;
				}


				bool CEmbeddedReasoner::checkConsistency(bool* consistentOut) {
					if (!mOntologyLoaded) {
						mLastError = "No ontology loaded.";
						return false;
					}
					CIsConsistentQueryCommand* command = new CIsConsistentQueryCommand(mKnowledgeBaseName);
					mPreconditionSynchronizer->delegateCommand(command);
					CCommandExecutedBlocker::waitExecutedCommand(command);
					return extractBooleanResult(command, consistentOut);
				}


				bool CEmbeddedReasoner::checkSatisfiability(const QString& classIRI, bool* satisfiableOut) {
					if (!mOntologyLoaded) {
						mLastError = "No ontology loaded.";
						return false;
					}
					CProcessClassNameSatisfiableQueryCommand* command = new CProcessClassNameSatisfiableQueryCommand(mKnowledgeBaseName, classIRI);
					mPreconditionSynchronizer->delegateCommand(command);
					CCommandExecutedBlocker::waitExecutedCommand(command);
					return extractBooleanResult(command, satisfiableOut);
				}


			}; // end namespace Embedded

		}; // end namespace Interface

	}; // end namespace Control

}; // end namespace Konclude

#endif // KONCLUDE_COMPILE_EMBEDDED_INTERFACE
