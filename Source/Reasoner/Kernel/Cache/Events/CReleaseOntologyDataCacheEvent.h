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

#ifndef KONCLUDE_REASONER_KERNEL_CACHE_EVENTS_CRELEASEONTOLOGYDATACACHEEVENT_H
#define KONCLUDE_REASONER_KERNEL_CACHE_EVENTS_CRELEASEONTOLOGYDATACACHEEVENT_H

// Libraries includes


// Namespace includes
#include "../CacheSettings.h"


// Other includes
#include "Concurrent/Events/CCustomEvent.h"
#include "Concurrent/Callback/CCallbackData.h"


// Logger includes
#include "Logger/CLogger.h"


namespace Konclude {

	using namespace Concurrent::Callback;

	namespace Reasoner {

		namespace Kernel {

			namespace Cache {

				namespace Events {

					/*!
					 *	\class		CReleaseOntologyDataCacheEvent
					 *	\brief		Asks a cache to drop the data it keeps for an ontology (identifier)
					 *				that is about to be deleted.
					 */
					class CReleaseOntologyDataCacheEvent : public Concurrent::Events::CCustomEvent {
						// public methods
						public:
							//! Constructor
							CReleaseOntologyDataCacheEvent(cint64 ontologyIdentifier, CCallbackData* callback);

							//! Destructor
							virtual ~CReleaseOntologyDataCacheEvent();

							cint64 getOntologyIdentifier();
							CCallbackData* getCallbackData();

							static const QEvent::Type EVENTTYPE = EVENTRELEASEONTOLOGYDATA;

						// protected variables
						protected:
							cint64 mOntologyIdentifier;
							CCallbackData* mCallbackData;

					};

				}; // end namespace Events

			}; // end namespace Cache

		}; // end namespace Kernel

	}; // end namespace Reasoner

}; // end namespace Konclude

#endif // KONCLUDE_REASONER_KERNEL_CACHE_EVENTS_CRELEASEONTOLOGYDATACACHEEVENT_H
