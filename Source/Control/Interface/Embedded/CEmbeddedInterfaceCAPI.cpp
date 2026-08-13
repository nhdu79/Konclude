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

#include "konclude_embedded.h"
#include "CEmbeddedReasoner.h"

#include <QString>

using namespace Konclude::Control::Interface::Embedded;


// Thin extern "C" shim: every function here just downcasts the opaque
// handle and forwards to CEmbeddedReasoner. No Konclude/Qt types cross
// this boundary -- see konclude_embedded.h for why (shared-library ABI
// stability, docs/FASTDOWNWARD_EMBEDDING.md phase 2).

extern "C" {

	KoncludeReasonerHandle konclude_create_reasoner(void) {
		return new CEmbeddedReasoner();
	}

	void konclude_destroy_reasoner(KoncludeReasonerHandle handle) {
		delete static_cast<CEmbeddedReasoner*>(handle);
	}

	int konclude_load_ontology_file(KoncludeReasonerHandle handle, const char* filePath) {
		if (!handle || !filePath) {
			return 0;
		}
		bool ok = static_cast<CEmbeddedReasoner*>(handle)->loadOntologyFile(QString::fromUtf8(filePath));
		return ok ? 1 : 0;
	}

	int konclude_check_consistency(KoncludeReasonerHandle handle, int* outConsistent) {
		if (!handle || !outConsistent) {
			return 0;
		}
		bool consistent = false;
		bool ok = static_cast<CEmbeddedReasoner*>(handle)->checkConsistency(&consistent);
		if (ok) {
			*outConsistent = consistent ? 1 : 0;
		}
		return ok ? 1 : 0;
	}

	int konclude_check_satisfiability(KoncludeReasonerHandle handle, const char* classIRI, int* outSatisfiable) {
		if (!handle || !classIRI || !outSatisfiable) {
			return 0;
		}
		bool satisfiable = false;
		bool ok = static_cast<CEmbeddedReasoner*>(handle)->checkSatisfiability(QString::fromUtf8(classIRI), &satisfiable);
		if (ok) {
			*outSatisfiable = satisfiable ? 1 : 0;
		}
		return ok ? 1 : 0;
	}

	const char* konclude_last_error(KoncludeReasonerHandle handle) {
		if (!handle) {
			return "";
		}
		return static_cast<CEmbeddedReasoner*>(handle)->getLastErrorCStr();
	}

} // extern "C"

#endif // KONCLUDE_COMPILE_EMBEDDED_INTERFACE
