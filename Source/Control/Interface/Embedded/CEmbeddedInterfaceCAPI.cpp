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
#include <QVector>

#include "CEmbeddedOntologyLoader.h"

using namespace Konclude::Control::Interface::Embedded;


namespace {

	// Copies a C array into Qt-side facts; false on a negative count, a missing
	// array or a null IRI string.
	bool copyClassAssertions(const KoncludeClassAssertion* assertions, int count, QVector<ClassAssertionFact>& facts) {
		if (count < 0 || (count > 0 && !assertions)) {
			return false;
		}
		facts.resize(count);
		for (int i = 0; i < count; ++i) {
			if (!assertions[i].individualIRI || !assertions[i].classIRI) {
				return false;
			}
			facts[i].individualIRI = QString::fromUtf8(assertions[i].individualIRI);
			facts[i].classIRI = QString::fromUtf8(assertions[i].classIRI);
		}
		return true;
	}

	bool copyObjectPropertyAssertions(const KoncludeObjectPropertyAssertion* assertions, int count, QVector<ObjectPropertyAssertionFact>& facts) {
		if (count < 0 || (count > 0 && !assertions)) {
			return false;
		}
		facts.resize(count);
		for (int i = 0; i < count; ++i) {
			if (!assertions[i].subjectIRI || !assertions[i].propertyIRI || !assertions[i].objectIRI) {
				return false;
			}
			facts[i].subjectIRI = QString::fromUtf8(assertions[i].subjectIRI);
			facts[i].propertyIRI = QString::fromUtf8(assertions[i].propertyIRI);
			facts[i].objectIRI = QString::fromUtf8(assertions[i].objectIRI);
		}
		return true;
	}

}


// Thin extern "C" shim: every function here just downcasts the opaque
// handle and forwards to CEmbeddedReasoner. No Konclude/Qt types cross
// this boundary -- see konclude_embedded.h for why (shared-library ABI
// stability, docs/EMBEDDED_INTERFACE.md).

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

	int konclude_tell_and_retract_assertions(KoncludeReasonerHandle handle,
			const KoncludeClassAssertion* tellClassAssertions, int tellClassAssertionCount,
			const KoncludeClassAssertion* retractClassAssertions, int retractClassAssertionCount,
			const KoncludeObjectPropertyAssertion* tellObjectPropertyAssertions, int tellObjectPropertyAssertionCount,
			const KoncludeObjectPropertyAssertion* retractObjectPropertyAssertions, int retractObjectPropertyAssertionCount) {
		if (!handle) {
			return 0;
		}
		CEmbeddedReasoner* reasoner = static_cast<CEmbeddedReasoner*>(handle);
		AssertionChangeSet changes;
		if (!copyClassAssertions(tellClassAssertions, tellClassAssertionCount, changes.tellClassAssertions) ||
				!copyClassAssertions(retractClassAssertions, retractClassAssertionCount, changes.retractClassAssertions) ||
				!copyObjectPropertyAssertions(tellObjectPropertyAssertions, tellObjectPropertyAssertionCount, changes.tellObjectPropertyAssertions) ||
				!copyObjectPropertyAssertions(retractObjectPropertyAssertions, retractObjectPropertyAssertionCount, changes.retractObjectPropertyAssertions)) {
			reasoner->setLastError("Invalid tell/retract arguments (negative count, missing array or null IRI string).");
			return 0;
		}
		bool ok = reasoner->tellAndRetractAssertions(changes);
		return ok ? 1 : 0;
	}

	int konclude_tell_and_retract_axioms(KoncludeReasonerHandle handle, const KoncludeClassAssertion* tellAssertions, int tellAssertionCount, const KoncludeClassAssertion* retractAssertions, int retractAssertionCount) {
		return konclude_tell_and_retract_assertions(handle, tellAssertions, tellAssertionCount, retractAssertions, retractAssertionCount, nullptr, 0, nullptr, 0);
	}

	int konclude_execute_conjunctive_query(KoncludeReasonerHandle handle, const char* sparqlSelectQuery) {
		if (!handle || !sparqlSelectQuery) {
			return 0;
		}
		bool ok = static_cast<CEmbeddedReasoner*>(handle)->executeConjunctiveQuery(QString::fromUtf8(sparqlSelectQuery));
		return ok ? 1 : 0;
	}

	int konclude_probe_scratch_revision_cycles(KoncludeReasonerHandle handle, int iterations) {
		if (!handle) {
			return -1;
		}
		return static_cast<CEmbeddedReasoner*>(handle)->probeScratchRevisionCycles(iterations);
	}

	int konclude_query_result_row_count(KoncludeReasonerHandle handle) {
		if (!handle) {
			return 0;
		}
		return static_cast<CEmbeddedReasoner*>(handle)->getLastQueryResultRowCount();
	}

	int konclude_query_result_variable_count(KoncludeReasonerHandle handle) {
		if (!handle) {
			return 0;
		}
		return static_cast<CEmbeddedReasoner*>(handle)->getLastQueryResultVariableCount();
	}

	const char* konclude_query_result_variable_name(KoncludeReasonerHandle handle, int varIndex) {
		if (!handle) {
			return "";
		}
		return static_cast<CEmbeddedReasoner*>(handle)->getLastQueryResultVariableNameCStr(varIndex);
	}

	const char* konclude_query_result_binding(KoncludeReasonerHandle handle, int row, int varIndex) {
		if (!handle) {
			return "";
		}
		return static_cast<CEmbeddedReasoner*>(handle)->getLastQueryResultBindingCStr(row, varIndex);
	}

	const char* konclude_last_error(KoncludeReasonerHandle handle) {
		if (!handle) {
			return "";
		}
		return static_cast<CEmbeddedReasoner*>(handle)->getLastErrorCStr();
	}

} // extern "C"

#endif // KONCLUDE_COMPILE_EMBEDDED_INTERFACE
