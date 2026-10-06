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

#ifndef KONCLUDE_REASONER_KERNEL_MANAGER_CREALIZATIONMANAGER_H
#define KONCLUDE_REASONER_KERNEL_MANAGER_CREALIZATIONMANAGER_H

// Libraries includes
#include <QReadWriteLock>
#include <QHash>

// Namespace includes
#include "ManagerSettings.h"

// Other includes
#include "Reasoner/Realizer/CRealizer.h"

#include "Reasoner/Ontology/CConcreteOntology.h"

#include "Config/CConfigurationProvider.h"

#include "Control/Command/CConfigManagerReader.h"

// Logger includes
#include "Logger/CLogger.h"



namespace Konclude {

	using namespace Config;
	using namespace Control::Command;

	namespace Reasoner {

		using namespace Realizer;
		using namespace Ontology;

		namespace Kernel {

			namespace Manager {


				/*! 
				 *
				 *		\class		CRealizationManager
				 *		\author		Andreas Steigmiller
				 *		\version	0.1
				 *		\brief		TODO
				 *
				 */
				class CRealizationManager {
					// public methods
					public:
						//! Constructor
						CRealizationManager(CReasonerManager* reasonerManager);

						//! Destructor
						virtual ~CRealizationManager();

						virtual CRealizer* getRealizer(CConcreteOntology *ontology, CConfigurationBase *config);

						//! Forgets the ontology and returns its realizer (or nullptr), which must then
						//! release its item for the ontology.
						virtual CRealizer* releaseOntology(CConcreteOntology *ontology);

						virtual CRealizationProgress* getRealizationProgress();

					// protected methods
					protected:

					// protected variables
					protected:
						CReasonerManager* mReasonerManager;
						QReadWriteLock mReadWriteLock;
						QHash<CConcreteOntology*,CRealizer*> mOntoRealizerHash;
						QSet<CRealizer*> mRealizerSet;
						//! One realizer (and thus one OS thread) shared by all ontologies that are not
						//! realized incrementally; realizer threads keep per-ontology items, and are never
						//! deleted, so creating one per ontology revision leaked one thread per revision.
						CRealizer* mSharedRealizer;
						CRealizer* mSharedIncrementalRealizer;

						CRealizationProgress mRealizationProgress;

					// private methods
					private:

					// private variables
					private:

				};

			}; // end namespace Manager

		}; // end namespace Kernel

	}; // end namespace Reasoner

}; // end namespace Konclude

#endif // KONCLUDE_REASONER_KERNEL_MANAGER_CREALIZATIONMANAGER_H
