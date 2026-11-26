/*
===========================================================================
openQAA: the BPK name hash.

Copyright (C) 2026 openQAA contributors.
This file is part of openQAA, licensed under the GNU GPL v2 or later.
===========================================================================
*/
#include "qaa_bpk.h"

/*
 * PLACEHOLDER (milestone M1).
 *
 * BPK records are keyed by a 64-bit hash of the file's path. The algorithm,
 * including how the path is normalised (case, separators, leading "baseq3/"),
 * is still being recovered from default.xex's filesystem code. Until it is
 * implemented here, every name hashes to 0, which matches no record, and
 * QAA_BpkNameHashKnown() returns 0.
 *
 * Meanwhile entries are reachable by raw hash (the qaa_bpkread command) and
 * by name through a name list with explicit hashes ("<16 hex digits> <path>"
 * lines, see fs_qaaNameList).
 *
 * When replacing this, keep the signature: QAA_BpkNameHash is the only place
 * that turns a path into a BPK key.
 */
uint64_t QAA_BpkNameHash( const char *path ) {
	(void)path;
	return 0;
}

int QAA_BpkNameHashKnown( void ) {
	return 0;
}
