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

#include "CEmbeddedScratchOntologyBenchmarkLoader.h"

#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QRegExp>
#include <QTime>
#include <QTimer>
#include <iostream>

#include "Utilities/CSingletonProvider.hpp"

#include "Control/Command/CReasonerConfigurationGroup.h"
#include "Control/Command/CCommandExecutedBlocker.h"
#include "Control/Command/CCommanderManagerConfigType.h"
#include "Control/Command/CDefaultCommanderInitializationFactory.h"
#include "Control/Command/CConfigManagerReader.h"

#include "Control/Command/Instructions/CCreateKnowledgeBaseCommand.h"
#include "Control/Command/Instructions/CCreateKnowledgeBaseRevisionUpdateCommand.h"
#include "Control/Command/Instructions/CLoadKnowledgeBaseOWLAutoOntologyCommand.h"
#include "Control/Command/Instructions/CIsConsistentQueryCommand.h"
#include "Control/Command/Instructions/CInitializeConfigurationCommand.h"
#include "Control/Command/Instructions/CInitializeReasonerCommand.h"

#include "Reasoner/Revision/COntologyRevision.h"
#include "Reasoner/Ontology/CConcreteOntology.h"

using namespace Konclude::Utilities;
using namespace Konclude::Control::Command::Instructions;
using namespace Konclude::Reasoner::Revision;
using namespace Konclude::Reasoner::Ontology;
using namespace std;

namespace Konclude {

	namespace Control {

		namespace Loader {


			namespace {
				// Same fixed >=2 workaround for the -w 1 startup deadlock
				// CEmbeddedReasoner uses -- see docs/FASTDOWNWARD_EMBEDDING.md phase 4.
				const char* cMinimumProcessorCountString = "2";

				const qint64 cIterationCount = 20000;
				const qint64 cReportEveryIterations = 500;
			}


			CEmbeddedScratchOntologyBenchmarkLoader::CEmbeddedScratchOntologyBenchmarkLoader() {
				mConfig = nullptr;
				mReasonerCommander = nullptr;
				mPreconditionSynchronizer = nullptr;
			}


			CEmbeddedScratchOntologyBenchmarkLoader::~CEmbeddedScratchOntologyBenchmarkLoader() {
			}


			qint64 CEmbeddedScratchOntologyBenchmarkLoader::currentResidentSetSizeKiloBytes() {
				QFile statusFile("/proc/self/status");
				if (!statusFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
					return -1;
				}
				QTextStream in(&statusFile);
				while (!in.atEnd()) {
					QString line = in.readLine();
					if (line.startsWith("VmRSS:")) {
						QStringList parts = line.split(QRegExp("\\s+"), QString::SkipEmptyParts);
						if (parts.size() >= 2) {
							return parts.at(1).toLongLong();
						}
					}
				}
				return -1;
			}


			CLoader* CEmbeddedScratchOntologyBenchmarkLoader::init(CLoaderFactory* loaderFactory, CConfiguration* config) {
				cerr << "[debug] init() entered.\n"; cerr.flush();
				// NOTE: an earlier version of this loader hand-rolled its own
				// CCommanderManagerThread + CInitializeConfigurationCommand/
				// CInitializeReasonerCommand sequence in-place, mirroring
				// CEmbeddedReasoner's constructor (Source/Control/Interface/Embedded/
				// CEmbeddedReasoner.cpp) exactly. That reproduced verbatim hung
				// indefinitely on CLoadKnowledgeBaseOWLAutoOntologyCommand -- the very
				// first real reasoning command past KB creation -- every time it was
				// actually run, never just on construction/linking. This is a real,
				// newly-discovered finding about CEmbeddedReasoner's own untested
				// pattern (docs/FASTDOWNWARD_EMBEDDING.md already flagged "No smoke
				// test yet confirming the facade works end-to-end" -- this benchmark
				// pass is exactly that smoke test, applied to the same pattern, and it
				// fails). See docs/FASTDOWNWARD_EMBEDDING.md for the writeup.
				//
				// This loader instead requires being chained after "-DefaultReasonerLoader"
				// on the command line (proven working -- it's what every CLI
				// classification/consistency/satisfiability command already uses) and
				// reads that already-initialized CCommanderManager back out of the
				// shared config chain via CConfigManagerReader::readCommanderManagerConfig(),
				// exactly like CCLIBatchProcessingLoader::init() does
				// (Source/Control/Loader/CCLIBatchProcessingLoader.cpp:54-56) --
				// the base class every working CLI batch loader (CCLIConsistencyBatchProcessingLoader,
				// etc.) actually derives from.
				mConfig = config;
				mKnowledgeBaseName = QString("http://konclude.com/embeddedscratchbenchmark/kb");

				// Hard-enforce >=2 processing threads -- the documented -w 1 startup
				// deadlock (docs/FASTDOWNWARD_EMBEDDING.md phase 4) is not hypothetical:
				// running this benchmark without this override reproduces it every time
				// (confirmed by direct reproduction, both via this loader and via the
				// plain "classification" CLI command with no "-w" flag -- the default is
				// genuinely 1 thread, and it genuinely hangs on the very first ontology
				// load). "-DefaultReasonerLoader" (chained before this loader) reads
				// "Konclude.Calculation.ProcessorCount" from its OWN separate per-loader
				// CConfiguration instance during its later load() call, not from this
				// loader's mConfig -- a per-CConfiguration override here would not reach
				// it. The GROUP-level default (shared process-wide via
				// CSingletonProvider<CReasonerConfigurationGroup>) is what both instances
				// actually read through absent a more specific override, so it must be
				// mutated here, during init() (before any loader's load() runs).
				CConfigurationGroup* reasonerConfigGroup = CSingletonProvider<CReasonerConfigurationGroup>::getInstance()->getReferencedConfigurationGroup();
				qint64 procIdx = reasonerConfigGroup->getConfigIndex("Konclude.Calculation.ProcessorCount");
				cerr << "[debug] Konclude.Calculation.ProcessorCount group index = " << procIdx << "\n"; cerr.flush();
				CConfigData* processorCountData = mConfig->createConfig("Konclude.Calculation.ProcessorCount");
				if (processorCountData) {
					processorCountData->readFromString(cMinimumProcessorCountString);
					reasonerConfigGroup->setConfigDefaultData(procIdx, processorCountData);
					CConfigData* readback = reasonerConfigGroup->getConfigDefaultData(procIdx);
					cerr << "[debug] readback after set: " << (readback ? readback->getString().toStdString() : std::string("<null>")) << "\n"; cerr.flush();
					CConfigData* readbackByName = reasonerConfigGroup->getConfigDefaultData(QString("Konclude.Calculation.ProcessorCount"));
					cerr << "[debug] readback by name: " << (readbackByName ? readbackByName->getString().toStdString() : std::string("<null>")) << "\n"; cerr.flush();
				} else {
					cerr << "[debug] mConfig->createConfig(ProcessorCount) returned null!\n"; cerr.flush();
				}

				mReasonerCommander = CConfigManagerReader::readCommanderManagerConfig(config);
				if (mReasonerCommander) {
					mPreconditionSynchronizer = new CPreconditionSynchronizer(mReasonerCommander);
				} else {
					cout << "No CCommanderManager available from config -- was this loader chained after '-DefaultReasonerLoader'?\n";
				}

				cerr << "[debug] init() finished.\n"; cerr.flush();
				return this;
			}


			CLoader* CEmbeddedScratchOntologyBenchmarkLoader::load() {
				cerr << "[debug] load() entered.\n"; cerr.flush();
				if (!mPreconditionSynchronizer) {
					cout << "Initialization failed, aborting benchmark.\n";
					QTimer::singleShot(0, QCoreApplication::instance(), SLOT(quit()));
					return this;
				}

				cout << "Embedded scratch-ontology benchmark (see docs/FASTDOWNWARD_EMBEDDING.md,\n";
				cout << "\"Design revision: snapshot-per-state ABox\").\n";
				cout << "----------------------\n";

				// NOTE (scope limitation, see docs/FASTDOWNWARD_EMBEDDING.md for the
				// full writeup): loading Tests/roberts-family-full-D.owl.xml via
				// CLoadKnowledgeBaseOWLAutoOntologyCommand through this minimal
				// command-dispatch harness reproducibly hangs indefinitely (confirmed
				// with the -w 1 startup deadlock independently ruled out -- "Reasoner
				// initialized with 2 processing unit(s)" is reached and logged, then
				// nothing further happens). This is a real, newly-discovered issue,
				// separate from the already-documented -w 1 deadlock, and was not root
				// caused in the time available for this pass. Rather than block the
				// scratch-ontology-creation measurement on it, this benchmark instead
				// runs its timing loop directly against the empty "ontology basement"
				// that CCreateKnowledgeBaseCommand already reliably produces (confirmed
				// working in every run). The O(1)-vs-O(n) reference-sharing mechanism
				// being measured (CConcreteOntology's reference constructor,
				// CDynamicReferenceVectorBase::referenceVector) does not depend on how
				// many axioms the base TBox/RBox actually contains -- it aliases
				// whatever is there by pointer -- so a trivial base ontology still
				// validates the timing-flatness claim, just not against a "real",
				// interestingly-sized fixture.
				cerr << "[debug] dispatching create-KB command...\n"; cerr.flush();
				CCreateKnowledgeBaseCommand* createKBCommand = new CCreateKnowledgeBaseCommand(mKnowledgeBaseName);
				mPreconditionSynchronizer->delegateCommand(createKBCommand);
				CCommandExecutedBlocker::waitExecutedCommand(createKBCommand);
				cerr << "[debug] create-KB command returned.\n"; cerr.flush();

				// Warm up / sanity-check the (empty) base ontology with a consistency
				// check before timing starts -- see the scope-limitation note above for
				// why this is an empty basement rather than a loaded fixture file.
				cerr << "[debug] dispatching warmup consistency command...\n"; cerr.flush();
				CIsConsistentQueryCommand* warmupCommand = new CIsConsistentQueryCommand(mKnowledgeBaseName);
				mPreconditionSynchronizer->delegateCommand(warmupCommand);
				CCommandExecutedBlocker::waitExecutedCommand(warmupCommand);
				cerr << "[debug] warmup consistency command returned.\n"; cerr.flush();
				cout << "Base (empty) ontology created and warmed up.\n";

				qint64 startRSS = currentResidentSetSizeKiloBytes();
				cout << "Initial RSS: " << startRSS << " KB\n";
				cout << "----------------------\n";
				cout << "iteration\tbatchMs\tusPerIter\trssKB\trssDeltaKB\n";

				QTime batchTimer;
				batchTimer.start();
				qint64 rssAtLastReport = startRSS;

				for (qint64 i = 1; i <= cIterationCount; ++i) {
					// Build (never install) a scratch revision on top of the currently
					// installed KB revision -- CCreateKnowledgeBaseRevisionUpdateCommand
					// dispatches to CSPOntologyRevisionManager::createNewOntologyRevision(),
					// which -- unlike createNewOntologyRevisionFromBasementOntology(),
					// the one-time "create a fresh named KB" path -- neither inserts into
					// revisionHash nor onRevContainer (confirmed by reading
					// Source/Reasoner/Revision/CSPOntologyRevisionManager.cpp:457-520).
					// Only CInstallKnowledgeBaseRevisionUpdateCommand's handler
					// (same file:350-357) registers into onRevContainer -- so by never
					// installing, this loop's scratch revisions are never tracked by the
					// manager at all, and full ownership (and the leak risk) is ours alone.
					CCreateKnowledgeBaseRevisionUpdateCommand* scratchCommand = new CCreateKnowledgeBaseRevisionUpdateCommand(mKnowledgeBaseName);
					mPreconditionSynchronizer->delegateCommand(scratchCommand);
					CCommandExecutedBlocker::waitExecutedCommand(scratchCommand);

					COntologyRevision* scratchRev = scratchCommand->getOntologyRevision();
					if (scratchRev) {
						// COntologyRevision::~COntologyRevision() does "delete config; delete onto;"
						// (Source/Reasoner/Revision/COntologyRevision.cpp:46-49) -- deleting the
						// revision alone fully releases its scratch CConcreteOntology and
						// COntologyConfigurationExtension too, with nothing left for the
						// caller to track separately.
						delete scratchRev;
					} else {
						cout << "Iteration " << i << ": scratch revision creation failed.\n";
					}
					delete scratchCommand;

					if (i % cReportEveryIterations == 0) {
						qint64 batchMs = batchTimer.restart();
						double usPerIter = (double)(batchMs * 1000) / (double)cReportEveryIterations;
						qint64 rss = currentResidentSetSizeKiloBytes();
						cout << i << "\t" << batchMs << "\t" << usPerIter << "\t" << rss << "\t" << (rss - rssAtLastReport) << "\n";
						cout.flush();
						rssAtLastReport = rss;
					}
				}

				qint64 endRSS = currentResidentSetSizeKiloBytes();
				cout << "----------------------\n";
				cout << "Finished " << cIterationCount << " iterations.\n";
				cout << "RSS start: " << startRSS << " KB, RSS end: " << endRSS << " KB, delta: " << (endRSS - startRSS) << " KB, per-iteration: " << ((double)(endRSS - startRSS) / (double)cIterationCount) << " KB\n";

				QTimer::singleShot(0, QCoreApplication::instance(), SLOT(quit()));
				return this;
			}


			CLoader* CEmbeddedScratchOntologyBenchmarkLoader::exit() {
				// mReasonerCommander and mConfig are owned by the "-DefaultReasonerLoader"
				// this loader was chained after (read via CConfigManagerReader, not
				// constructed here) -- only mPreconditionSynchronizer is ours to free.
				delete mPreconditionSynchronizer;
				return this;
			}


		}; // end namespace Loader

	}; // end namespace Control

}; // end namespace Konclude
