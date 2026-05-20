//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Fixed point arithemtics, implementation.
//


#ifndef __M_FIXED__
#define __M_FIXED__

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>




//
// Fixed point, 32bit as 16.16.
//
#define FRACBITS		16
#define FRACUNIT		(1<<FRACBITS)

typedef int fixed_t;

#ifndef M_FIXED_NO_INLINE
static inline fixed_t FixedMul(fixed_t a, fixed_t b)
{
    return ((int64_t) a * (int64_t) b) >> FRACBITS;
}
#else
fixed_t FixedMul	(fixed_t a, fixed_t b);
#endif

#ifndef M_FIXED_NO_INLINE
static inline fixed_t FixedDiv(fixed_t a, fixed_t b)
{
    if ((abs(a) >> 14) >= abs(b))
    {
	return (a^b) < 0 ? INT_MIN : INT_MAX;
    }
    else
    {
	int64_t result;

	result = ((int64_t) a << FRACBITS) / b;

	return (fixed_t) result;
    }
}
#else
fixed_t FixedDiv	(fixed_t a, fixed_t b);
#endif



#endif
