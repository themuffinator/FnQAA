/*
===========================================================================
FnQAA: Quake Arena Arcade's default bindings on the PC.

Copyright (C) 2026 FnQAA contributors.
This file is part of FnQAA, licensed under the GNU GPL v2 or later.
===========================================================================
*/
#ifndef QAA_BINDS_H
#define QAA_BINDS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PC deviation: QAA's configs bind the controller under the key names the
 * Xbox 360 engine gives its pad, and the PC has a keyboard and a mouse too.
 *
 * Retail sends the pad's A, B, X, Y, shoulder, Back, Start and stick buttons
 * as JOY1..JOY10, its right and left triggers as MOUSE1 and MOUSE2 and its
 * D-pad as the arrow keys, as the comments in the package's control schemes
 * (controlschemes/*.cfg) name them. The PC reports the pad under its own keys
 * (PAD0_*), so the engine renames those keys in the bind and unbind commands
 * of QAA's default.cfg and control schemes as it executes them. The keyboard
 * and the mouse then take Quake Live's defaults, from FnQAA's fnqaa.cfg
 * (pkg/baseq3/fnqaa.cfg, shipped in FnQAA-pkg.fnz), which the engine runs
 * after default.cfg without renaming its keys.
 */

/* True for a file whose binds are QAA's pad binds: default.cfg and
 * controlschemes/<name>.cfg, as exec names them (either slash, any case). */
int QAA_IsPadConfig( const char *filename );

/*
 * Copies config text to out (outSize bytes, always terminated when outSize is
 * not 0), renaming the keys of QAA's pad in its bind and unbind commands.
 * Returns the length of the whole rewritten text, so a call with out NULL
 * sizes the buffer.
 */
size_t QAA_RewritePadConfig( const char *text, char *out, size_t outSize );

/*
 * The same for command text from the front end (its FSCommand "qcmd": the
 * Custom Controls menu binds the pad under the retail names, as its
 * keybind_btn_* cvars hold them). Its unbindall, which clears the Xbox's
 * only bindings, the pad's, unbinds the pad's keys alone, so the keyboard,
 * the mouse and the sticks keep theirs.
 */
size_t QAA_RewriteFrontEndCommands( const char *text, char *out, size_t outSize );

#ifdef __cplusplus
}
#endif

#endif /* QAA_BINDS_H */
