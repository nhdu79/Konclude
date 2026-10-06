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

#ifndef KONCLUDE_REASONER_PREPROCESS_EVENTS_CRELEASEPREPROCESSINGONTOLOGYEVENT_H
#define KONCLUDE_REASONER_PREPROCESS_EVENTS_CRELEASEPREPROCESSINGONTOLOGYEVENT_H

// Libraries includes


// Namespace includes
#include "../PreprocessSettings.h"


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

		namespace Preprocess {

			namespace Events {

				/*!
				 *	\class		CReleasePreprocessingOntologyEvent
				 *	\brief		Asks a preprocessing thread to drop and delete its item for an
				 *				ontology that is about to be deleted.
				 */
				class CReleasePreprocessingOntologyEvent : public CCustomEvent {
					// public methods
					public:
						//! Constructor
						CReleasePreprocessingOntologyEvent(CConcreteOntology* ontology, CCallbackData* callback);

						//! Destructor
						virtual ~CReleasePreprocessingOntologyEvent();

						CConcreteOntology* getOntology();
						CCallbackData* getCallbackData();

						static const QEvent::Type EVENTTYPE = EVENTRELEASEPREPROCESSINGONTOLOGY;

					// protected variables
					protected:
						CConcreteOntology* mOntology;
						CCallbackData* mCallbackData;

				};

			}; // end namespace Events

		}; // end namespace Preprocess

	}; // end namespace Reasoner

}; // end namespace Konclude

#endif // KONCLUDE_REASONER_PREPROCESS_EVENTS_CRELEASEPREPROCESSINGONTOLOGYEVENT_H
