/*
===========================================================================
FnQAA: Quake Arena Arcade .bpk archive names.

Copyright (C) 2026 FnQAA contributors.
This file is part of FnQAA, licensed under the GNU GPL v2 or later.
===========================================================================
*/
#ifndef QAA_BPK_H
#define QAA_BPK_H

#include <stddef.h>
#include <stdint.h>

#if defined( _WIN32 ) && defined( QAA_LZX_SHARED )
#define QAA_BPK_API __declspec( dllexport )
#else
#define QAA_BPK_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The 64-bit name hash a .bpk record is keyed by, for a path relative to the
 * game directory ("maps/q3dm17.bsp"). Separators and case do not matter.
 */
QAA_BPK_API uint64_t qaa_bpk_name_hash( const char *path );

/*
 * Hash `count` NUL-separated names packed back to back in `names`, writing
 * each hash to `out`. For tools that test many candidate names.
 */
QAA_BPK_API void qaa_bpk_name_hash_many( const char *names, size_t count, uint64_t *out );

#ifdef __cplusplus
}
#endif

#endif
