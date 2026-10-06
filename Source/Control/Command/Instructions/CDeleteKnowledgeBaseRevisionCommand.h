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

#ifndef KONCLUDE_CONTROL_COMMAND_INSTRUCTIONS_CDELETEKNOWLEDGEBASEREVISIONCOMMAND_H
#define KONCLUDE_CONTROL_COMMAND_INSTRUCTIONS_CDELETEKNOWLEDGEBASEREVISIONCOMMAND_H

// Libraries includes
#include <QString>

// Namespace includes
#include "CKnowledgeBaseCommand.h"

// Other includes
#include "Reasoner/Revision/COntologyRevision.h"
#include "Control/Command/CommandSettings.h"

// Logger includes
#include "Logger/CLogger.h"


namespace Konclude {

	using namespace Reasoner::Revision;

	namespace Control {

		namespace Command {

			namespace Instructions {

				/*!
				 *		\class		CDeleteKnowledgeBaseRevisionCommand
				 *		\brief		Removes an installed, superseded (i.e., not the current) revision of a
				 *					knowledge base from the revision manager and deletes it. The reasoning
				 *					data for its ontology must have been released before (see
				 *					CReleaseOntologyCommand) and no other revision may still refer to it.
				 */
				class CDeleteKnowledgeBaseRevisionCommand : public CKnowledgeBaseCommand {
					// public methods
					public:
						CDeleteKnowledgeBaseRevisionCommand(const QString& knowledgeBaseNameString, COntologyRevision* ontRevision, CCommand* parentSuperCommand = 0);
						virtual ~CDeleteKnowledgeBaseRevisionCommand();

						virtual qint64 getCommandTag();
						virtual QString getBriefCommandDescription();

						virtual QString getKnowledgeBaseName();
						COntologyRevision* getOntologyRevision();

					// protected variables
					protected:
						QString mKBString;
						COntologyRevision* mOntRevision;

				};

			}; // end namespace Instructions

		}; // end namespace Command

	}; // end namespace Control

}; // end namespace Konclude

#endif // KONCLUDE_CONTROL_COMMAND_INSTRUCTIONS_CDELETEKNOWLEDGEBASEREVISIONCOMMAND_H
