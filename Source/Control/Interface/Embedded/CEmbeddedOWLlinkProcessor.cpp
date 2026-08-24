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

#include "CEmbeddedOWLlinkProcessor.h"

#include "Control/Command/CConfigManagerReader.h"
#include "Control/Command/CCommanderManager.h"
#include "Control/Command/CCommandExecutedBlocker.h"
#include "Control/Command/Instructions/CGetDescriptionCommand.h"


namespace Konclude {

	namespace Control {

		namespace Interface {

			namespace Embedded {


				CEmbeddedOWLlinkProcessor::CEmbeddedOWLlinkProcessor(CConfiguration* loaderConfig) : COWLlinkProcessor(false) {
					mLoaderConfig = loaderConfig;
					reasonerCommander = CConfigManagerReader::readCommanderManagerConfig(mLoaderConfig);
					startThread();
				}


				CEmbeddedOWLlinkProcessor::~CEmbeddedOWLlinkProcessor() {
				}


				COWLlinkProcessor* CEmbeddedOWLlinkProcessor::initializeOWLlinkContent() {
					return this;
				}


				COWLlinkProcessor* CEmbeddedOWLlinkProcessor::concludeOWLlinkContent() {
					return this;
				}


				CConfiguration* CEmbeddedOWLlinkProcessor::getConfiguration() {
					if (!reasonerCommander) {
						return mLoaderConfig;
					} else {
						CGetDescriptionCommand* getDesComm = new CGetDescriptionCommand();
						reasonerCommander->delegateCommand(getDesComm);
						CCommandExecutedBlocker commExeBlocker;
						commExeBlocker.waitExecutedCommand(getDesComm);
						CConfiguration* config = getDesComm->getConfiguration();
						return config;
					}
				}


			}; // end namespace Embedded

		}; // end namespace Interface

	}; // end namespace Control

}; // end namespace Konclude

#endif // KONCLUDE_COMPILE_EMBEDDED_INTERFACE
