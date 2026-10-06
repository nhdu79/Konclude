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

#ifndef KONCLUDE_REASONER_KERNEL_MANAGER_EVENTS_CRELEASEONTOLOGYEVENT_H
#define KONCLUDE_REASONER_KERNEL_MANAGER_EVENTS_CRELEASEONTOLOGYEVENT_H

// Libraries includes


// Namespace includes
#include "../ManagerSettings.h"


// Other includes
#include "Concurrent/Events/CCustomEvent.h"
#include "Concurrent/Callback/CCallbackData.h"

#include "Reasoner/Ontology/CConcreteOntology.h"


// Logger includes
#include "Logger/CLogger.h"


namespace Konclude {

	using namespace Concurrent::Events;
	using namespace Concurrent::Callback;

	namespace Reasoner {

		using namespace Ontology;

		namespace Kernel {

			namespace Manager {

				namespace Events {

					/*!
					 *	\class		CReleaseOntologyEvent
					 *	\brief		Asks the reasoner manager to drop all reasoning data it and its
					 *				components keep for an ontology that is about to be deleted.
					 */
					class CReleaseOntologyEvent : public CCustomEvent {
						// public methods
						public:
							//! Constructor
							CReleaseOntologyEvent(CConcreteOntology* ontology, CCallbackData* callback);

							//! Destructor
							virtual ~CReleaseOntologyEvent();

							CConcreteOntology* getOntology();
							CCallbackData* getCallbackData();

							static const QEvent::Type EVENTTYPE = EVENTRELEASEONTOLOGY;

						// protected variables
						protected:
							CConcreteOntology* mOntology;
							CCallbackData* mCallbackData;

					};

				}; // end namespace Events

			}; // end namespace Manager

		}; // end namespace Kernel

	}; // end namespace Reasoner

}; // end namespace Konclude

#endif // KONCLUDE_REASONER_KERNEL_MANAGER_EVENTS_CRELEASEONTOLOGYEVENT_H
