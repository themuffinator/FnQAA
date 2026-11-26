/*
===========================================================================
Copyright (C) 2026 FnQL contributors

This file is part of FnQL and is distributed under the terms of the GNU
General Public License version 2 or (at your option) any later version.
===========================================================================
*/

#ifndef CVAR_RETAIL_BOUNDS_H
#define CVAR_RETAIL_BOUNDS_H

#include "q_shared.h"

#include <stdlib.h>

/*
 * Range check for CV_RETAIL_BOUNDED cvars, the cvars retail Quake Live
 * registers with a minimum and maximum (the cgame and qagame bounded
 * register imports, and the engine's com_maxfps).
 *
 * Evidence: QLSRP's retail cvar code (Cvar_GetValidatedValueString) converts
 * the new string with atof and compares it with the float bounds. A value
 * below or above the range is replaced by the bound's string; every other
 * string is stored verbatim, including one with trailing text such as
 * "250x".  Quake III style CV_INTEGER / CV_FLOAT validation instead rejects
 * a string that is not wholly numeric and resets it to the default, which
 * broke retail configs that set com_maxfps "250x".
 *
 * The number is the leading numeric prefix, as atof reads it; a non-finite
 * result counts as 0, like Q_atof, so "inf" and "nan" cannot defeat the range.
 */
typedef enum {
	CVAR_RETAIL_IN_RANGE = 0,
	CVAR_RETAIL_BELOW_MIN,
	CVAR_RETAIL_ABOVE_MAX
} cvarRetailBound_t;

static ID_INLINE float Cvar_RetailNumber( const char *s )
{
	float value;

	if ( !s ) {
		return 0.0f;
	}

	value = (float)atof( s );
	if ( value - value != 0.0f ) {	// NaN or infinity
		return 0.0f;
	}
	return value;
}

static ID_INLINE cvarRetailBound_t Cvar_RetailBoundsCheck( const char *value,
	const char *mins, const char *maxs )
{
	const float number = Cvar_RetailNumber( value );

	if ( mins && number < Cvar_RetailNumber( mins ) ) {
		return CVAR_RETAIL_BELOW_MIN;
	}
	if ( maxs && number > Cvar_RetailNumber( maxs ) ) {
		return CVAR_RETAIL_ABOVE_MAX;
	}
	return CVAR_RETAIL_IN_RANGE;
}

#endif // CVAR_RETAIL_BOUNDS_H
