/*
===========================================================================
Copyright (C) 2026 FnQL contributors

This file is part of FnQL and is distributed under the terms of the GNU
General Public License version 2 or (at your option) any later version.
===========================================================================
*/

#include "../code/qcommon/cvar_integer.h"

#include <stdio.h>

static int failures;

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
		failures++; \
	} \
} while (0)

/* Issue #11: the retail cgame reads packed RGBA colour cvars as integers. */
static void TestRetailHexColours(void) {
	CHECK(Cvar_ParseInteger("0x2a8000FF") == 0x2a8000FF);
	CHECK(Cvar_ParseInteger("0x00FF00FF") == 0x00FF00FF);
	CHECK(Cvar_ParseInteger("0xffffffff") == (int)0xffffffffu);
	CHECK(Cvar_ParseInteger("0xFF0000FF") == (int)0xFF0000FFu);
	CHECK(Cvar_ParseInteger("0x10") == 16);
	CHECK(Cvar_ParseInteger("0x0") == 0);
}

/* Retail reads at most eight hex digits and defaults to 0 when none parse. */
static void TestRetailHexLimits(void) {
	CHECK(Cvar_ParseInteger("0x123456789") == 0x12345678);
	CHECK(Cvar_ParseInteger("0x0000000000000000") == 0);
	CHECK(Cvar_ParseInteger("0x") == 0);
	CHECK(Cvar_ParseInteger("0xZZ") == 0);
	CHECK(Cvar_ParseInteger("0x1fZZ") == 0x1f);
}

/* Strings outside the hex prefix keep the Quake III atoi result. */
static void TestDecimalUnchanged(void) {
	CHECK(Cvar_ParseInteger(NULL) == 0);
	CHECK(Cvar_ParseInteger("") == 0);
	CHECK(Cvar_ParseInteger("0") == 0);
	CHECK(Cvar_ParseInteger("1") == 1);
	CHECK(Cvar_ParseInteger("-25") == -25);
	CHECK(Cvar_ParseInteger("  12") == 12);
	CHECK(Cvar_ParseInteger("1.75") == 1);
	CHECK(Cvar_ParseInteger("010") == 10);
	CHECK(Cvar_ParseInteger("keel/bright") == 0);
	CHECK(Cvar_ParseInteger("1920x1080") == 1920);
	CHECK(Cvar_ParseInteger("10x") == 10);
	/* Retail and atoi agree on 0 for these, so neither takes the hex path. */
	CHECK(Cvar_ParseInteger(" 0x10") == 0);
	CHECK(Cvar_ParseInteger("-0x10") == 0);
	CHECK(Cvar_ParseInteger("0X10") == 0);
}

/*
 * CV_RETAIL_BOUNDED cvars use retail's exact helper: "0x" anywhere selects
 * the hex parse, which converts nothing unless the string starts with it.
 */
static void TestRetailExactParse(void) {
	CHECK(Cvar_ParseIntegerRetail(NULL) == 0);
	CHECK(Cvar_ParseIntegerRetail("") == 0);
	CHECK(Cvar_ParseIntegerRetail("125") == 125);
	CHECK(Cvar_ParseIntegerRetail("250") == 250);
	CHECK(Cvar_ParseIntegerRetail("1.75") == 1);
	CHECK(Cvar_ParseIntegerRetail("-25") == -25);
	/* The com_maxfps "250x" trick: the string contains "0x". */
	CHECK(Cvar_ParseIntegerRetail("250x") == 0);
	CHECK(Cvar_ParseIntegerRetail("100x") == 0);
	CHECK(Cvar_ParseIntegerRetail("1920x1080") == 0);
	/* No "0x" in the string: plain atoi, trailing text ignored. */
	CHECK(Cvar_ParseIntegerRetail("125x") == 125);
	CHECK(Cvar_ParseIntegerRetail("10 x") == 10);
	/* A leading "0x" parses as hex exactly like Cvar_ParseInteger. */
	CHECK(Cvar_ParseIntegerRetail("0x2a8000FF") == 0x2a8000FF);
	CHECK(Cvar_ParseIntegerRetail("0x123456789") == 0x12345678);
	CHECK(Cvar_ParseIntegerRetail("0x") == 0);
	CHECK(Cvar_ParseIntegerRetail("0x1fZZ") == 0x1f);
	/* The engine-safe variant is unchanged for the same strings. */
	CHECK(Cvar_ParseInteger("250x") == 250);
	CHECK(Cvar_ParseInteger("1920x1080") == 1920);
}

int main(void) {
	TestRetailHexColours();
	TestRetailHexLimits();
	TestDecimalUnchanged();
	TestRetailExactParse();

	if (failures) {
		fprintf(stderr, "%d cvar integer checks failed\n", failures);
		return 1;
	}
	printf("cvar integer checks passed\n");
	return 0;
}
