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

/*
 * Public C API for embedding Konclude in-process as a shared library
 * (KoncludeEmbedded.pro, KONCLUDE_COMPILE_EMBEDDED_INTERFACE). Deliberately
 * a plain C surface -- no Qt types, no C++ types -- so it stays ABI-stable
 * across compilers/toolchains for callers linking libKonclude.so/.dylib
 * dynamically. See docs/EMBEDDED_INTERFACE.md for the design rationale.
 *
 * Thread-safety: a single KoncludeReasonerHandle is NOT safe to call from
 * multiple threads concurrently. Sequential use of separate handles, one
 * at a time (create, use, destroy, then create the next), is supported.
 * Genuinely concurrent, simultaneously-alive handles are NOT yet verified
 * safe -- the CCommanderManager wiring goes through a process-wide shared
 * config slot internally (see CEmbeddedReasoner.cpp for detail) -- so
 * don't rely on that until it's been tested.
 */

#ifndef KONCLUDE_EMBEDDED_H
#define KONCLUDE_EMBEDDED_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void* KoncludeReasonerHandle;

typedef struct {
  const char* individualIRI;
  const char* classIRI;
} KoncludeClassAssertion;

/* ObjectPropertyAssertion(propertyIRI, subjectIRI, objectIRI). */
typedef struct {
  const char* subjectIRI;
  const char* propertyIRI;
  const char* objectIRI;
} KoncludeObjectPropertyAssertion;

/*
 * Creates a new reasoner instance. Internally forces >=2 processing
 * threads to avoid a known deadlock in Konclude's startup synchronization
 * that is triggered by single-threaded configuration -- never exposed as
 * a caller-settable option. Returns NULL on failure.
 *
 * At most one instance may be alive at a time: two live instances share the
 * commander registered in Konclude's process-wide configuration and can
 * deadlock. To replace an instance, destroy the old one first.
 */
KoncludeReasonerHandle konclude_create_reasoner(void);

/*
 * Destroys a reasoner instance created by konclude_create_reasoner: stops
 * its threads and frees its reasoning data. Memory leaked by state changes
 * is only freed at process exit; to bound it, run Konclude in a worker
 * process that is restarted (Tools/KoncludeWorker/ in the Konclude repo).
 * Passing NULL is a no-op.
 */
void konclude_destroy_reasoner(KoncludeReasonerHandle handle);

/*
 * Loads an OWL2-XML or OWL2-Functional-Syntax ontology from a local file
 * path into the reasoner. May be called only once per instance -- create
 * a new instance to reason over a different ontology. Returns 1 on
 * success, 0 on failure (see konclude_last_error).
 */
int konclude_load_ontology_file(KoncludeReasonerHandle handle, const char* filePath);

/*
 * Runs a consistency check on the loaded ontology. Returns 1 if the call
 * succeeded (result written to *outConsistent, 1 = consistent), 0 on
 * failure.
 */
int konclude_check_consistency(KoncludeReasonerHandle handle, int* outConsistent);

/*
 * Runs a class satisfiability check. classIRI must be a fully-qualified
 * IRI string. Returns 1 if the call succeeded (result written to
 * *outSatisfiable, 1 = satisfiable), 0 on failure.
 */
int konclude_check_satisfiability(KoncludeReasonerHandle handle, const char* classIRI, int* outSatisfiable);

/*
 * Moves the knowledge base to the next state: retracts every retract*
 * assertion, then tells every tell* assertion (so a fact in both ends up
 * told), as one incremental revision -- the same thing an OWLlink
 * <Retract>+<Tell> request does. Pass only the diff between the previous
 * and the next state; retracting a fact that is not asserted is a no-op.
 * Any array may be NULL if its count is 0. Reasoning is lazy: nothing is
 * computed until the next query. IRIs must be fully-qualified. Returns 1
 * on success, 0 on failure (see konclude_last_error); on failure nothing
 * is changed.
 *
 * Memory note: every call that changes something keeps one more revision
 * alive inside Konclude (revision cleanup is not implemented), so memory
 * grows with the number of state changes.
 */
int konclude_tell_and_retract_assertions(KoncludeReasonerHandle handle,
    const KoncludeClassAssertion* tellClassAssertions, int tellClassAssertionCount,
    const KoncludeClassAssertion* retractClassAssertions, int retractClassAssertionCount,
    const KoncludeObjectPropertyAssertion* tellObjectPropertyAssertions, int tellObjectPropertyAssertionCount,
    const KoncludeObjectPropertyAssertion* retractObjectPropertyAssertions, int retractObjectPropertyAssertionCount);

/*
 * Same as konclude_tell_and_retract_assertions with class assertions only.
 */
int konclude_tell_and_retract_axioms(KoncludeReasonerHandle handle, const KoncludeClassAssertion* tellAssertions, int tellAssertionCount, const KoncludeClassAssertion* retractAssertions, int retractAssertionCount);

/*
 * Runs a SPARQL SELECT query with a single basic graph pattern (the
 * non-Rasqal query engine, see docs/CONJUNCTIVE_QUERY_PIPELINE.md) against
 * the current state. Results are readable via konclude_query_result_*
 * until the next call. Returns 1 on success, 0 on failure (see
 * konclude_last_error).
 */
int konclude_execute_conjunctive_query(KoncludeReasonerHandle handle, const char* sparqlSelectQuery);

/*
 * Diagnostic probe, not part of the stable API surface: repeatedly builds
 * a scratch knowledge-base revision and immediately discards it without
 * ever installing it (see docs/EMBEDDED_INTERFACE.md §4). Prints one progress line per iteration to
 * stderr. Returns the number of iterations that completed; run this under
 * a shell-level timeout when checking for a hang, since a hung call never
 * returns at all. Requires an ontology already loaded via
 * konclude_load_ontology_file. Returns -1 if no ontology is loaded.
 */
int konclude_probe_scratch_revision_cycles(KoncludeReasonerHandle handle, int iterations);

/*
 * Number of result rows / projected variables from the most recent
 * konclude_execute_conjunctive_query call. 0 if no query has been run yet
 * or the last one failed.
 */
int konclude_query_result_row_count(KoncludeReasonerHandle handle);
int konclude_query_result_variable_count(KoncludeReasonerHandle handle);

/*
 * Name of the varIndex-th projected variable (0-based, matches the
 * column order used by konclude_query_result_binding). Returns an empty
 * string if varIndex is out of range. The returned pointer is owned by
 * the reasoner instance -- copy it if it needs to outlive the next call.
 */
const char* konclude_query_result_variable_name(KoncludeReasonerHandle handle, int varIndex);

/*
 * Bound value (as a string -- an IRI, blank node label, or literal) for
 * the given row/variable of the most recent conjunctive query result.
 * Returns an empty string if row/varIndex is out of range. The returned
 * pointer is owned by the reasoner instance -- copy it if it needs to
 * outlive the next call.
 */
const char* konclude_query_result_binding(KoncludeReasonerHandle handle, int row, int varIndex);

/*
 * Returns the most recent error/exception/catastrophic log message
 * recorded for this reasoner instance, or an empty string if none. The
 * returned pointer is owned by the reasoner instance -- copy it if it
 * needs to outlive the next call on the same handle.
 */
const char* konclude_last_error(KoncludeReasonerHandle handle);

#ifdef __cplusplus
}
#endif

#endif // KONCLUDE_EMBEDDED_H
