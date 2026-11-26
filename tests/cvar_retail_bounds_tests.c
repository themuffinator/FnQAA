/*
===========================================================================
Copyright (C) 2026 FnQL contributors

This file is part of FnQL and is distributed under the terms of the GNU
General Public License version 2 or (at your option) any later version.
===========================================================================
*/

#include "../code/qcommon/cvar_retail_bounds.h"
#include "../code/qcommon/cvar_integer.h"

#include <stdio.h>

static int failures;

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
		failures++; \
	} \
} while (0)

/* Retail compares the atof prefix with the bounds and keeps the string. */
static void TestNumericPrefix(void) {
	CHECK(Cvar_RetailNumber(NULL) == 0.0f);
	CHECK(Cvar_RetailNumber("") == 0.0f);
	CHECK(Cvar_RetailNumber("250") == 250.0f);
	CHECK(Cvar_RetailNumber("250x") == 250.0f);
	CHECK(Cvar_RetailNumber("0.75") == 0.75f);
	CHECK(Cvar_RetailNumber("abc") == 0.0f);
	CHECK(Cvar_RetailNumber("-20") == -20.0f);
	/* Non-finite results count as 0, as Q_atof does. */
	CHECK(Cvar_RetailNumber("1e999") == 0.0f);
	CHECK(Cvar_RetailNumber("inf") == 0.0f);
	CHECK(Cvar_RetailNumber("nan") == 0.0f);
}

static void TestBounds(void) {
	CHECK(Cvar_RetailBoundsCheck("125", "30", "250") == CVAR_RETAIL_IN_RANGE);
	CHECK(Cvar_RetailBoundsCheck("30", "30", "250") == CVAR_RETAIL_IN_RANGE);
	CHECK(Cvar_RetailBoundsCheck("250", "30", "250") == CVAR_RETAIL_IN_RANGE);
	CHECK(Cvar_RetailBoundsCheck("29", "30", "250") == CVAR_RETAIL_BELOW_MIN);
	CHECK(Cvar_RetailBoundsCheck("251", "30", "250") == CVAR_RETAIL_ABOVE_MAX);
	CHECK(Cvar_RetailBoundsCheck("250.5", "30", "250") == CVAR_RETAIL_ABOVE_MAX);
	CHECK(Cvar_RetailBoundsCheck("abc", "30", "250") == CVAR_RETAIL_BELOW_MIN);
	CHECK(Cvar_RetailBoundsCheck("abc", "0", "1") == CVAR_RETAIL_IN_RANGE);
	CHECK(Cvar_RetailBoundsCheck("inf", "30", "250") == CVAR_RETAIL_BELOW_MIN);
	/* Float endpoints and fractional values stay float comparisons. */
	CHECK(Cvar_RetailBoundsCheck("0.75", "0", "1") == CVAR_RETAIL_IN_RANGE);
	CHECK(Cvar_RetailBoundsCheck("1.01", "0", "1") == CVAR_RETAIL_ABOVE_MAX);
	/* A missing bound is not enforced. */
	CHECK(Cvar_RetailBoundsCheck("5000", "0", NULL) == CVAR_RETAIL_IN_RANGE);
	CHECK(Cvar_RetailBoundsCheck("-5000", NULL, "1") == CVAR_RETAIL_IN_RANGE);
	CHECK(Cvar_RetailBoundsCheck("-5000", NULL, NULL) == CVAR_RETAIL_IN_RANGE);
}

/*
 * com_maxfps "250x": retail keeps the string under both the cgame's 30..250
 * registration and FnQL's engine 0..1000 one, and its integer is 0, which the
 * frame limiter treats as no cap. "300x" is clamped to the cgame maximum.
 */
static void TestMaxFpsTrick(void) {
	CHECK(Cvar_RetailBoundsCheck("250x", "30", "250") == CVAR_RETAIL_IN_RANGE);
	CHECK(Cvar_RetailBoundsCheck("250x", "0", "1000") == CVAR_RETAIL_IN_RANGE);
	CHECK(Cvar_ParseIntegerRetail("250x") == 0);
	CHECK(Cvar_RetailBoundsCheck("300x", "30", "250") == CVAR_RETAIL_ABOVE_MAX);
	/* Plain values keep their integer, so the limiter still applies. */
	CHECK(Cvar_ParseIntegerRetail("250") == 250);
	CHECK(Cvar_ParseIntegerRetail("125") == 125);
}

int main(void) {
	TestNumericPrefix();
	TestBounds();
	TestMaxFpsTrick();

	if (failures) {
		fprintf(stderr, "%d cvar retail bounds checks failed\n", failures);
		return 1;
	}
	printf("cvar retail bounds checks passed\n");
	return 0;
}
