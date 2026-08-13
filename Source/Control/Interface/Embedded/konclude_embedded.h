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
