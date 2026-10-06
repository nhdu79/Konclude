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

#ifndef KONCLUDE_CONTROL_COMMAND_INSTRUCTIONS_CRELEASEONTOLOGYCOMMAND_H
#define KONCLUDE_CONTROL_COMMAND_INSTRUCTIONS_CRELEASEONTOLOGYCOMMAND_H

// Libraries includes
#include <QString>

// Namespace includes
#include "Control/Command/CommandSettings.h"
#include "Control/Command/CPreconditionCommand.h"

// Other includes
#include "Reasoner/Ontology/CConcreteOntology.h"

// Logger includes
#include "Logger/CLogger.h"


namespace Konclude {

	using namespace Reasoner::Ontology;

	namespace Control {

		namespace Command {

			namespace Instructions {

				/*!
				 *		\class		CReleaseOntologyCommand
				 *		\brief		Makes the reasoner drop all reasoning data it keeps for an
				 *					ontology (items of the preprocessing, precomputation, ...
				 *					threads), so that the ontology can be deleted afterwards. The
				 *					ontology itself is not deleted by this command.
				 */
				class CReleaseOntologyCommand : public CPreconditionCommand {
					// public methods
					public:
						CReleaseOntologyCommand(CConcreteOntology* ontology, CCommand* parentSuperCommand = 0);
						virtual ~CReleaseOntologyCommand();

						virtual QString getBriefCommandDescription();
						virtual qint64 getCommandTag();

						CConcreteOntology* getOntology();

					// protected variables
					protected:
						CConcreteOntology* mOntology;

				};

			}; // end namespace Instructions

		}; // end namespace Command

	}; // end namespace Control

}; // end namespace Konclude

#endif // KONCLUDE_CONTROL_COMMAND_INSTRUCTIONS_CRELEASEONTOLOGYCOMMAND_H
