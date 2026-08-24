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

#ifndef KONCLUDE_Control_LOADER_CEMBEDDEDSCRATCHONTOLOGYBENCHMARKLOADER_H
#define KONCLUDE_Control_LOADER_CEMBEDDEDSCRATCHONTOLOGYBENCHMARKLOADER_H

// Libraries includes
#include <QString>

// Namespace includes
#include "CLoader.h"

// Other includes
#include "Config/CConfiguration.h"
#include "Control/Command/CCommanderManagerThread.h"
#include "Control/Command/CPreconditionSynchronizer.h"

// Logger includes
#include "Logger/CLogger.h"


namespace Konclude {

	using namespace Logger;
	using namespace Config;
	using namespace Control::Command;

	namespace Control {

		namespace Loader {

			/*!
			 *
			 *		\class		CEmbeddedScratchOntologyBenchmarkLoader
			 *		\brief		Standalone benchmark for the FD-embedding "snapshot-per-state
			 *					ABox" design (see docs/FASTDOWNWARD_EMBEDDING.md). Loads and
			 *					classifies a fixture ontology once, then repeatedly builds an
			 *					uninstalled scratch COntologyRevision on top of it via
			 *					CCreateKnowledgeBaseRevisionUpdateCommand (never installing it),
			 *					measuring per-iteration wall time and process RSS to check
			 *					whether TBox/RBox reference-sharing keeps creation flat-cost and
			 *					whether skipping the install step avoids the CSPOntologyRevisionManager
			 *					onRevContainer leak.
			 *
			 *					Invoke with: ./Konclude -DefaultReasonerLoader "+Konclude.Calculation.ProcessorCount=2" -EmbeddedScratchOntologyBenchmarkLoader
			 *					(must be chained after -DefaultReasonerLoader, which is what
			 *					actually constructs and initializes the CCommanderManager this
			 *					loader reads back out of the shared config chain; the quoted
			 *					"+Konclude.Calculation.ProcessorCount=2" argument is required to
			 *					avoid the documented -w 1 startup deadlock, since this direct
			 *					loader-name invocation bypasses the "-w" CLI flag translator)
			 *
			 *		\note		KNOWN BLOCKED, as of this writing: this loader currently hangs
			 *					indefinitely on the warmup CIsConsistentQueryCommand (i.e. it
			 *					never reaches its timing loop). This is a newly-discovered bug,
			 *					not the already-documented -w 1 deadlock (reproduced with that
			 *					independently ruled out -- "Reasoner initialized with 2
			 *					processing unit(s)" is reached and logged). Root cause: any
			 *					command dispatched via CPreconditionSynchronizer wrapping a raw
			 *					CCommanderManager/CCommanderManagerThread (exactly the pattern
			 *					CEmbeddedReasoner also uses, Source/Control/Interface/Embedded/
			 *					CEmbeddedReasoner.cpp) succeeds for CCreateKnowledgeBaseCommand
			 *					but hangs for every command after it; the identical command types
			 *					work correctly when dispatched through COWLlinkProcessor's own
			 *					delegateCommand() (Source/Control/Interface/OWLlink/
			 *					COWLlinkProcessor.cpp:69-72), which every proven CLI/OWLlink path
			 *					actually goes through. Not root-caused further in the pass that
			 *					added this file -- see docs/FASTDOWNWARD_EMBEDDING.md for the
			 *					full writeup and reproduction steps. Left in the repo as-is
			 *					(reproducing the bug on demand) rather than deleted, since it is
			 *					itself the diagnostic artifact.
			 */
			class CEmbeddedScratchOntologyBenchmarkLoader : public CLoader {
				// public methods
				public:
					//! Constructor
					CEmbeddedScratchOntologyBenchmarkLoader();

					//! Destructor
					virtual ~CEmbeddedScratchOntologyBenchmarkLoader();

					virtual CLoader* init(CLoaderFactory* loaderFactory = 0, CConfiguration* config = 0);
					virtual CLoader* load();
					virtual CLoader* exit();

				// protected methods
				protected:
					static qint64 currentResidentSetSizeKiloBytes();

				// protected variables
				protected:
					CConfiguration* mConfig;
					CCommanderManager* mReasonerCommander;
					CPreconditionSynchronizer* mPreconditionSynchronizer;
					QString mKnowledgeBaseName;

				// private methods
				private:

				// private variables
				private:

			};

		}; // end namespace Loader

	}; // end namespace Control

}; // end namespace Konclude

#endif // KONCLUDE_Control_LOADER_CEMBEDDEDSCRATCHONTOLOGYBENCHMARKLOADER_H
