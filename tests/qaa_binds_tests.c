/*
===========================================================================
Copyright (C) 2026 FnQAA contributors

This file is part of FnQAA and is distributed under the terms of the GNU
General Public License version 2 or (at your option) any later version.
===========================================================================
*/

#include "../code/qcommon/qaa_binds.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
		failures++; \
	} \
} while (0)

static char out[4096];

static const char *Rewrite(const char *text) {
	const size_t len = QAA_RewritePadConfig(text, NULL, 0);
	CHECK(len < sizeof(out));
	CHECK(QAA_RewritePadConfig(text, out, sizeof(out)) == len);
	CHECK(strlen(out) == len);
	return out;
}

#define REWRITES(in, expected) do { \
	const char *got = Rewrite(in); \
	if (strcmp(got, expected) != 0) { \
		fprintf(stderr, "%s:%d: '%s' became '%s', expected '%s'\n", __FILE__, __LINE__, in, got, expected); \
		failures++; \
	} \
} while (0)

static void TestPadConfigNames(void) {
	CHECK(QAA_IsPadConfig("default.cfg"));
	CHECK(QAA_IsPadConfig("Default.CFG"));
	CHECK(QAA_IsPadConfig("controlschemes\\default.cfg"));
	CHECK(QAA_IsPadConfig("controlschemes/_dpadquick.cfg"));
	CHECK(QAA_IsPadConfig("ControlSchemes/basic.cfg"));
	CHECK(!QAA_IsPadConfig("controlschemes/"));
	CHECK(!QAA_IsPadConfig("controlschemesx/basic.cfg"));
	CHECK(!QAA_IsPadConfig("autoexec.cfg"));
	CHECK(!QAA_IsPadConfig("qzconfig.cfg"));
	CHECK(!QAA_IsPadConfig("cfg/default.cfg"));
	CHECK(!QAA_IsPadConfig(NULL));
}

/* Retail's names (controlschemes/default.cfg comments) become the PC's. */
static void TestPadKeys(void) {
	REWRITES("bind JOY1 \"lastweap;+button11;\" // A button\n",
		"bind PAD0_SOUTH \"lastweap;+button11;\" // A button\n");
	REWRITES("bind JOY2 \"spin180\"", "bind PAD0_EAST \"spin180\"");
	REWRITES("bind JOY3 weapprev", "bind PAD0_WEST weapprev");
	REWRITES("bind JOY4 weapnext", "bind PAD0_NORTH weapnext");
	REWRITES("bind JOY5 +zoom", "bind PAD0_LEFTSHOULDER +zoom");
	REWRITES("bind JOY6 +button2", "bind PAD0_RIGHTSHOULDER +button2");
	REWRITES("bind JOY7 scores", "bind PAD0_BACK scores");
	REWRITES("bind JOY8 pause", "bind PAD0_START pause");
	REWRITES("bind JOY9 +movedown", "bind PAD0_LEFTSTICK_CLICK +movedown");
	REWRITES("bind JOY10 centerview", "bind PAD0_RIGHTSTICK_CLICK centerview");
	REWRITES("bind MOUSE1 \"+attack\"       // right trigger",
		"bind PAD0_RIGHTTRIGGER \"+attack\"       // right trigger");
	REWRITES("bind MOUSE2 \"+moveup;+button12;\"", "bind PAD0_LEFTTRIGGER \"+moveup;+button12;\"");
	REWRITES("bind UPARROW \"vstr heavyweapons\"", "bind PAD0_DPAD_UP \"vstr heavyweapons\"");
	REWRITES("bind  DOWNARROW   +back", "bind  PAD0_DPAD_DOWN   +back");
	REWRITES("bind\tLEFTARROW\t+left", "bind\tPAD0_DPAD_LEFT\t+left");
	REWRITES("unbind RIGHTARROW\r\n", "unbind PAD0_DPAD_RIGHT\r\n");
	REWRITES("unbind JOY9", "unbind PAD0_LEFTSTICK_CLICK");
	REWRITES("BIND joy1 x", "BIND PAD0_SOUTH x");
	REWRITES("bind \"JOY1\" x", "bind \"PAD0_SOUTH\" x");
}

static void TestUntouched(void) {
	REWRITES("", "");
	REWRITES("unbindall\n", "unbindall\n");
	REWRITES("bind JOY11 x", "bind JOY11 x");
	REWRITES("bind w +forward", "bind w +forward");
	REWRITES("bind MOUSE3 +button2", "bind MOUSE3 +button2");
	REWRITES("//bind  MOUSE1  +attack\n", "//bind  MOUSE1  +attack\n");
	REWRITES("  // bind JOY1 x; bind JOY2 y\n", "  // bind JOY1 x; bind JOY2 y\n");
	REWRITES("bindlist JOY1", "bindlist JOY1");
	REWRITES("echo bind JOY1", "echo bind JOY1");
	REWRITES("set shortrange \"weapon 2;bind JOY1 x\"", "set shortrange \"weapon 2;bind JOY1 x\"");
	REWRITES("bind", "bind");
	REWRITES("bind JOY1", "bind PAD0_SOUTH");
}

/* Commands split at newlines and unquoted semicolons, as Cbuf_Execute does. */
static void TestCommandSplitting(void) {
	REWRITES("echo a; bind JOY1 x;unbind JOY2\nbind MOUSE1 +attack",
		"echo a; bind PAD0_SOUTH x;unbind PAD0_EAST\nbind PAD0_RIGHTTRIGGER +attack");
	REWRITES("set a \"x\nbind JOY1 y", "set a \"x\nbind PAD0_SOUTH y");
	REWRITES("exec controlschemes\\_dpadquick.cfg\n", "exec controlschemes\\_dpadquick.cfg\n");
}

static void TestTruncation(void) {
	char small[8];
	CHECK(QAA_RewritePadConfig("bind JOY1 x", small, sizeof(small)) == strlen("bind PAD0_SOUTH x"));
	CHECK(strcmp(small, "bind PA") == 0);
	CHECK(QAA_RewritePadConfig("bind JOY1 x", small, 0) == strlen("bind PAD0_SOUTH x"));
	CHECK(QAA_RewritePadConfig(NULL, small, sizeof(small)) == 0 && small[0] == '\0');
}

static const char *RewriteFrontEnd(const char *text) {
	const size_t len = QAA_RewriteFrontEndCommands(text, NULL, 0);
	CHECK(len < sizeof(out));
	CHECK(QAA_RewriteFrontEndCommands(text, out, sizeof(out)) == len);
	CHECK(strlen(out) == len);
	return out;
}

#define FRONT_END_REWRITES(in, expected) do { \
	const char *got = RewriteFrontEnd(in); \
	if (strcmp(got, expected) != 0) { \
		fprintf(stderr, "%s:%d: '%s' became '%s', expected '%s'\n", __FILE__, __LINE__, in, got, expected); \
		failures++; \
	} \
} while (0)

#define PAD_UNBIND_ALL "unbind PAD0_SOUTH;unbind PAD0_EAST;unbind PAD0_WEST;unbind PAD0_NORTH;" \
	"unbind PAD0_LEFTSHOULDER;unbind PAD0_RIGHTSHOULDER;unbind PAD0_BACK;unbind PAD0_START;" \
	"unbind PAD0_LEFTSTICK_CLICK;unbind PAD0_RIGHTSTICK_CLICK;unbind PAD0_RIGHTTRIGGER;" \
	"unbind PAD0_LEFTTRIGGER;unbind PAD0_DPAD_UP;unbind PAD0_DPAD_DOWN;unbind PAD0_DPAD_LEFT;" \
	"unbind PAD0_DPAD_RIGHT"

/* The Custom Controls menu's commands (customcontrols.gfx: completeSetKeyBind,
 * unbindButton, unbindAll), as its FSCommand qcmd sends them. */
static void TestFrontEndCommands(void) {
	FRONT_END_REWRITES("bind JOY1 \"+moveup;+button11\";", "bind PAD0_SOUTH \"+moveup;+button11\";");
	FRONT_END_REWRITES("bind MOUSE2 \"+zoom\";", "bind PAD0_LEFTTRIGGER \"+zoom\";");
	FRONT_END_REWRITES("bind UPARROW \"vstr heavyweapons\";", "bind PAD0_DPAD_UP \"vstr heavyweapons\";");
	FRONT_END_REWRITES("unbind JOY10;", "unbind PAD0_RIGHTSTICK_CLICK;");
	FRONT_END_REWRITES("unbindall;", PAD_UNBIND_ALL ";");
	FRONT_END_REWRITES("unbindall", PAD_UNBIND_ALL);
	FRONT_END_REWRITES("UNBINDALL\n", PAD_UNBIND_ALL "\n");
	FRONT_END_REWRITES("bind JOY7 scores;", "bind PAD0_BACK scores;");
	FRONT_END_REWRITES("exec controlschemes\\sniper.cfg", "exec controlschemes\\sniper.cfg");
	FRONT_END_REWRITES("disconnect;qnet_disconnect;", "disconnect;qnet_disconnect;");
	FRONT_END_REWRITES("set closecombat \"weapon 11;unbindall\" ", "set closecombat \"weapon 11;unbindall\" ");
	FRONT_END_REWRITES("unbindallx", "unbindallx");
	/* an exec'd config keeps its own unbindall, which default.cfg follows
	 * with the keyboard's and the mouse's binds */
	REWRITES("unbindall;", "unbindall;");
}

/* Quake Live's keyboard and mouse defaults, with QAA's commands. */
static void TestPcBindings(void) {
	CHECK(strstr(QAA_PC_BINDINGS, "bind w +forward\n") != NULL);
	CHECK(strstr(QAA_PC_BINDINGS, "bind MOUSE1 +attack\n") != NULL);
	CHECK(strstr(QAA_PC_BINDINGS, "bind MOUSE2 +zoom\n") != NULL);
	CHECK(strstr(QAA_PC_BINDINGS, "bind f lastweap\n") != NULL);
	CHECK(strstr(QAA_PC_BINDINGS, "bind KP_PGUP \"follow 9\"\n") != NULL);
	CHECK(strstr(QAA_PC_BINDINGS, "bind PAD0_LEFTSTICK_UP +forward\n") != NULL);
	CHECK(strstr(QAA_PC_BINDINGS, "PAD0_RIGHTSTICK") == NULL);	/* the look stick is unbound */
	CHECK(strstr(QAA_PC_BINDINGS, "drop") == NULL);
	CHECK(strstr(QAA_PC_BINDINGS, "weapon 14") == NULL);
	CHECK(strstr(QAA_PC_BINDINGS, "JOY") == NULL);
	CHECK(strstr(QAA_PC_BINDINGS, "ARROW +") == NULL);	// the arrows are the D-pad's
	/* it binds the mouse, so it runs after the renaming, never through it */
	CHECK(strstr(Rewrite(QAA_PC_BINDINGS), "bind PAD0_RIGHTTRIGGER +attack\n") != NULL);
}

int main(void) {
	TestPadConfigNames();
	TestPadKeys();
	TestUntouched();
	TestCommandSplitting();
	TestTruncation();
	TestFrontEndCommands();
	TestPcBindings();
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	puts("qaa_binds tests passed");
	return 0;
}
