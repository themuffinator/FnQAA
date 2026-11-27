/*
===========================================================================
FnQAA: Quake Arena Arcade's default bindings on the PC.

Copyright (C) 2026 FnQAA contributors.
This file is part of FnQAA, licensed under the GNU GPL v2 or later.
===========================================================================
*/
#include "qaa_binds.h"

#include <string.h>

/* The retail pad key names (controlschemes/default.cfg comments) and the PC's. */
static const struct {
	const char *retail;
	const char *pc;
} padKeys[] = {
	{ "JOY1", "PAD0_SOUTH" },					// A
	{ "JOY2", "PAD0_EAST" },					// B
	{ "JOY3", "PAD0_WEST" },					// X
	{ "JOY4", "PAD0_NORTH" },					// Y
	{ "JOY5", "PAD0_LEFTSHOULDER" },
	{ "JOY6", "PAD0_RIGHTSHOULDER" },
	{ "JOY7", "PAD0_BACK" },
	{ "JOY8", "PAD0_START" },
	{ "JOY9", "PAD0_LEFTSTICK_CLICK" },
	{ "JOY10", "PAD0_RIGHTSTICK_CLICK" },
	{ "MOUSE1", "PAD0_RIGHTTRIGGER" },
	{ "MOUSE2", "PAD0_LEFTTRIGGER" },
	{ "UPARROW", "PAD0_DPAD_UP" },
	{ "DOWNARROW", "PAD0_DPAD_DOWN" },
	{ "LEFTARROW", "PAD0_DPAD_LEFT" },
	{ "RIGHTARROW", "PAD0_DPAD_RIGHT" },
};

/*
 * Quake Live's default.cfg for the keyboard and the mouse, with QAA's
 * commands where QL's differ. Left out, as QAA has no such command: the
 * arrows' dropflag, dropweapon, droppowerup and droprune, F3's readyup,
 * h's +chat, v's +voice, p's +acc and PAUSE's pause; and 0's "weapon 14",
 * the heavy machinegun, which QAA does not have.
 */
const char QAA_PC_BINDINGS[] =
	"// QAA: PC deviation: the keyboard and the mouse, as Quake Live's default.cfg\n"
	"bind w +forward\n"
	"bind s +back\n"
	"bind a +moveleft\n"
	"bind d +moveright\n"
	"bind SPACE +moveup\n"
	"bind c +movedown\n"
	"bind SHIFT +speed\n"
	"bind MOUSE1 +attack\n"
	"bind f lastweap\n"							// QL: "weapon toggle"
	"bind ] weapnext\n"
	"bind MWHEELDOWN weapnext\n"
	"bind [ weapprev\n"
	"bind MWHEELUP weapprev\n"
	"bind MOUSE2 +zoom\n"
	"bind ENTER +button2\n"
	"bind MOUSE3 +button2\n"
	"bind g +button3\n"
	"bind k kill\n"
	"bind TAB +scores\n"
	"bind F1 \"vote yes\"\n"
	"bind F2 \"vote no\"\n"
	"bind F11 screenshotJPEG\n"
	"bind t messagemode\n"
	"bind y messagemode2\n"
	"bind ESCAPE togglemenu\n"
	"bind ~ toggleconsole\n"
	"bind ` toggleconsole\n"
	"bind 1 \"weapon 1\"\n"
	"bind 2 \"weapon 2\"\n"
	"bind 3 \"weapon 3\"\n"
	"bind 4 \"weapon 4\"\n"
	"bind 5 \"weapon 5\"\n"
	"bind 6 \"weapon 6\"\n"
	"bind 7 \"weapon 7\"\n"
	"bind 8 \"weapon 8\"\n"
	"bind 9 \"weapon 9\"\n"
	"bind MOUSE4 \"weapon 10\"\n"
	"bind DEL +lookdown\n"
	"bind PGDN +lookup\n"
	"bind END centerview\n"
	"bind KP_INS \"follow 0\"\n"
	"bind KP_END \"follow 1\"\n"
	"bind KP_DOWNARROW \"follow 2\"\n"
	"bind KP_PGDN \"follow 3\"\n"
	"bind KP_LEFTARROW \"follow 4\"\n"
	"bind KP_5 \"follow 5\"\n"
	"bind KP_RIGHTARROW \"follow 6\"\n"
	"bind KP_HOME \"follow 7\"\n"
	"bind KP_UPARROW \"follow 8\"\n"
	"bind KP_PGUP \"follow 9\"\n"
	// Retail moves and looks with the sticks in the engine, unbound
	// (cl_southpaw 0); the PC binds them, and in_joystickUseAnalog makes
	// these binds analog.
	"// QAA: PC deviation: the pad's sticks, which retail reads unbound\n"
	"bind PAD0_LEFTSTICK_UP +forward\n"
	"bind PAD0_LEFTSTICK_DOWN +back\n"
	"bind PAD0_LEFTSTICK_LEFT +moveleft\n"
	"bind PAD0_LEFTSTICK_RIGHT +moveright\n"
	"bind PAD0_RIGHTSTICK_UP +lookup\n"
	"bind PAD0_RIGHTSTICK_DOWN +lookdown\n"
	"bind PAD0_RIGHTSTICK_LEFT +left\n"
	"bind PAD0_RIGHTSTICK_RIGHT +right\n";

static int LowerAscii( int c ) {
	return c >= 'A' && c <= 'Z' ? c + ( 'a' - 'A' ) : c;
}

/* A case-insensitive comparison of n bytes of s with the whole of word. */
static int WordIs( const char *s, size_t n, const char *word ) {
	size_t i;

	if ( strlen( word ) != n ) {
		return 0;
	}
	for ( i = 0; i < n; i++ ) {
		if ( LowerAscii( (unsigned char)s[i] ) != LowerAscii( (unsigned char)word[i] ) ) {
			return 0;
		}
	}
	return 1;
}

int QAA_IsPadConfig( const char *filename ) {
	static const char dir[] = "controlschemes";
	const size_t dirLen = sizeof( dir ) - 1;

	if ( !filename ) {
		return 0;
	}
	if ( WordIs( filename, strlen( filename ), "default.cfg" ) ) {
		return 1;
	}
	return strlen( filename ) > dirLen + 1 && WordIs( filename, dirLen, dir )
		&& ( filename[dirLen] == '/' || filename[dirLen] == '\\' );
}

typedef struct {
	char *out;
	size_t size;
	size_t len;
} writer_t;

static void Emit( writer_t *w, const char *s, size_t n ) {
	size_t i;

	for ( i = 0; i < n; i++, w->len++ ) {
		if ( w->out && w->len + 1 < w->size ) {
			w->out[w->len] = s[i];
		}
	}
}

static int IsBlank( char c ) {
	return c == ' ' || c == '\t' || c == '\r';
}

/*
 * The text is split into commands as Cbuf_Execute splits it: at newlines and
 * at semicolons outside quotes, with // comments running to the end of the
 * line. A command whose first word is bind or unbind has its key renamed.
 */
size_t QAA_RewritePadConfig( const char *text, char *out, size_t outSize ) {
	writer_t w = { out, outSize, 0 };
	const char *p = text ? text : "";
	int start = 1;
	int quoted = 0;

	while ( *p ) {
		if ( start ) {
			const char *word, *key;
			size_t wordLen, keyLen, i;
			int keyQuoted;

			start = 0;
			while ( IsBlank( *p ) ) {
				Emit( &w, p++, 1 );
			}
			if ( p[0] == '/' && p[1] == '/' ) {
				continue;	// a comment, copied below
			}
			word = p;
			while ( *p && !IsBlank( *p ) && *p != '\n' && *p != ';' && *p != '"' ) {
				p++;
			}
			wordLen = (size_t)( p - word );
			Emit( &w, word, wordLen );
			if ( !IsBlank( *p ) || !( WordIs( word, wordLen, "bind" ) || WordIs( word, wordLen, "unbind" ) ) ) {
				continue;
			}
			while ( IsBlank( *p ) ) {
				Emit( &w, p++, 1 );
			}
			keyQuoted = *p == '"';
			key = p + keyQuoted;
			for ( p = key; *p && *p != '\n' && ( keyQuoted ? *p != '"' : !IsBlank( *p ) && *p != ';' ); p++ ) {
			}
			keyLen = (size_t)( p - key );
			if ( keyQuoted ) {
				Emit( &w, "\"", 1 );
			}
			for ( i = 0; i < sizeof( padKeys ) / sizeof( padKeys[0] ); i++ ) {
				if ( WordIs( key, keyLen, padKeys[i].retail ) ) {
					break;
				}
			}
			if ( i < sizeof( padKeys ) / sizeof( padKeys[0] ) ) {
				Emit( &w, padKeys[i].pc, strlen( padKeys[i].pc ) );
			} else {
				Emit( &w, key, keyLen );
			}
			if ( keyQuoted && *p == '"' ) {
				Emit( &w, p++, 1 );
			}
			continue;
		}
		if ( !quoted && p[0] == '/' && p[1] == '/' ) {
			while ( *p && *p != '\n' ) {
				Emit( &w, p++, 1 );
			}
			continue;
		}
		if ( *p == '"' ) {
			quoted = !quoted;
		} else if ( *p == '\n' ) {
			quoted = 0;
			start = 1;
		} else if ( *p == ';' && !quoted ) {
			start = 1;
		}
		Emit( &w, p++, 1 );
	}
	if ( out && outSize ) {
		out[w.len < outSize ? w.len : outSize - 1] = '\0';
	}
	return w.len;
}
