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
 * dynamically. See docs/FASTDOWNWARD_EMBEDDING.md for the design rationale.
 *
 * Thread-safety: a single KoncludeReasonerHandle is NOT safe to call from
 * multiple threads concurrently. Sequential use of separate handles, one
 * at a time (create, use, destroy, then create the next), is supported.
 * Genuinely concurrent, simultaneously-alive handles are NOT yet verified
 * safe -- the CCommanderManager wiring goes through a process-wide shared
 * config slot internally (see CEmbeddedReasoner.cpp for detail) -- so
 * don't rely on that until it's been tested (tracked as a phase 7 item in
 * docs/FASTDOWNWARD_EMBEDDING.md).
 */

#ifndef KONCLUDE_EMBEDDED_H
#define KONCLUDE_EMBEDDED_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void* KoncludeReasonerHandle;

/*
 * Creates a new reasoner instance. Internally forces >=2 processing
 * threads to avoid a known deadlock in Konclude's startup synchronization
 * that is triggered by single-threaded configuration -- never exposed as
 * a caller-settable option. Returns NULL on failure.
 */
KoncludeReasonerHandle konclude_create_reasoner(void);

/*
 * Destroys a reasoner instance created by konclude_create_reasoner.
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
 * Starts a new FD "state": discards whatever scratch ABox revision was
 * current (a plain in-process free -- it was never installed, see
 * docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md's Decision 2) and creates a
 * fresh one layered on the loaded/classified base ontology. Call this once
 * per FD state, before asserting that state's ABox facts via
 * konclude_state_assert_class_fact. Requires an ontology already loaded via
 * konclude_load_ontology_file. Returns 1 on success, 0 on failure (see
 * konclude_last_error).
 */
int konclude_state_begin(KoncludeReasonerHandle handle);

/*
 * Asserts one ClassAssertion(individualIRI, classIRI) ground fact into the
 * current state (see konclude_state_begin). individualIRI/classIRI must be
 * fully-qualified IRI strings. Requires konclude_state_begin to have been
 * called first. Returns 1 on success, 0 on failure (see konclude_last_error).
 */
int konclude_state_assert_class_fact(KoncludeReasonerHandle handle, const char* individualIRI, const char* classIRI);

/*
 * Runs a SPARQL SELECT conjunctive query (single basic graph pattern --
 * several atoms joined by shared variables; OPTIONAL/UNION/nested SELECT
 * are not supported, see docs/CONJUNCTIVE_QUERY_PIPELINE.md) against the
 * current state (see konclude_state_begin/konclude_state_assert_class_fact
 * -- multiple queries against the same state see each other and every
 * asserted fact), using the same non-Rasqal query engine as Konclude's
 * `sparqlfile`/`sparqlserver` CLI commands. Requires konclude_state_begin
 * to have been called first. Returns 1 on success, 0 on failure (see
 * konclude_last_error). On success, the result rows are retrievable via
 * the konclude_query_result_* functions below until the next call to this
 * function on the same handle.
 */
int konclude_execute_conjunctive_query(KoncludeReasonerHandle handle, const char* sparqlSelectQuery);

/*
 * Diagnostic probe, not part of the stable API surface: repeatedly builds
 * a scratch knowledge-base revision and immediately discards it without
 * ever installing it (see docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md's
 * Decision 2/Decision 4). Prints one progress line per iteration to
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
