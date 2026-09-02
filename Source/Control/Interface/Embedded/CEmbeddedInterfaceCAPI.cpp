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

	int konclude_state_begin(KoncludeReasonerHandle handle) {
		if (!handle) {
			return 0;
		}
		bool ok = static_cast<CEmbeddedReasoner*>(handle)->beginNewState();
		return ok ? 1 : 0;
	}

	int konclude_state_assert_class_fact(KoncludeReasonerHandle handle, const char* individualIRI, const char* classIRI) {
		if (!handle || !individualIRI || !classIRI) {
			return 0;
		}
		bool ok = static_cast<CEmbeddedReasoner*>(handle)->assertClassFact(QString::fromUtf8(individualIRI), QString::fromUtf8(classIRI));
		return ok ? 1 : 0;
	}

	int konclude_state_begin_chained(KoncludeReasonerHandle handle) {
		if (!handle) {
			return 0;
		}
		bool ok = static_cast<CEmbeddedReasoner*>(handle)->beginNewChainedState();
		return ok ? 1 : 0;
	}

	int konclude_state_assert_class_fact_chained(KoncludeReasonerHandle handle, const char* individualIRI, const char* classIRI) {
		if (!handle || !individualIRI || !classIRI) {
			return 0;
		}
		bool ok = static_cast<CEmbeddedReasoner*>(handle)->assertClassFactChained(QString::fromUtf8(individualIRI), QString::fromUtf8(classIRI));
		return ok ? 1 : 0;
	}

	int konclude_state_retract_class_fact_chained(KoncludeReasonerHandle handle, const char* individualIRI, const char* classIRI) {
		if (!handle || !individualIRI || !classIRI) {
			return 0;
		}
		bool ok = static_cast<CEmbeddedReasoner*>(handle)->retractClassFactChained(QString::fromUtf8(individualIRI), QString::fromUtf8(classIRI));
		return ok ? 1 : 0;
	}

	int konclude_execute_conjunctive_query_chained(KoncludeReasonerHandle handle, const char* sparqlSelectQuery) {
		if (!handle || !sparqlSelectQuery) {
			return 0;
		}
		bool ok = static_cast<CEmbeddedReasoner*>(handle)->executeChainedConjunctiveQuery(QString::fromUtf8(sparqlSelectQuery));
		return ok ? 1 : 0;
	}

	int konclude_chained_query_result_row_count(KoncludeReasonerHandle handle) {
		if (!handle) {
			return 0;
		}
		return static_cast<CEmbeddedReasoner*>(handle)->getChainedQueryResultRowCount();
	}

	int konclude_chained_query_result_variable_count(KoncludeReasonerHandle handle) {
		if (!handle) {
			return 0;
		}
		return static_cast<CEmbeddedReasoner*>(handle)->getChainedQueryResultVariableCount();
	}

	const char* konclude_chained_query_result_variable_name(KoncludeReasonerHandle handle, int varIndex) {
		if (!handle) {
			return "";
		}
		return static_cast<CEmbeddedReasoner*>(handle)->getChainedQueryResultVariableNameCStr(varIndex);
	}

	const char* konclude_chained_query_result_binding(KoncludeReasonerHandle handle, int row, int varIndex) {
		if (!handle) {
			return "";
		}
		return static_cast<CEmbeddedReasoner*>(handle)->getChainedQueryResultBindingCStr(row, varIndex);
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
