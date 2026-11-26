/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
#ifndef VM_LOCAL_H
#define VM_LOCAL_H

#include "q_shared.h"
#include "qcommon.h"

// QAA: every module is hosted by the engine (VM_CreateHosted): QAA's
// recompiled qagame and cgame. There is no QVM bytecode and no native module
// loader, so a vm_t is only the entry point and the hooks around it.
struct vm_s {
	const char	*name;				// "qagame" or "cgame"
	vmIndex_t	index;

	vmMainFunc_t entryPoint;
	void		(*destroy)( vm_t *self );
	vm_t		*(*restart)( vm_t *self );

	char		providerName[32];

	int			callLevel;			// counts recursive VM_Call
	int			syscallCount;		// syscall counter for current VM_Call invocation

	int			privateFlag;
};

#endif // VM_LOCAL_H
