/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2012-2020 Quake3e project

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
// vm.c -- the game modules the engine hosts

/*
QAA: Quake Arena Arcade's qagame and cgame are recompiled from the user's
package and hosted by the engine (sv_qaa_game.inc, cl_qaa_cgame.inc), which
register them here with VM_CreateHosted. Quake III's QVM interpreter and
compilers and Quake Live's native module loader are gone: QAA ships no QVMs,
and its only modules are its own (docs/PLAN.md).
*/

#include "vm_local.h"

// used by Com_Error to get rid of running vm's before longjmp
static int forced_unload;

static struct vm_s vmTable[ VM_COUNT ];

static const char *vmName[ VM_COUNT ] = {
	"qagame",
#ifndef USE_DEDICATED
	"cgame",
#endif
};

static void VM_VmInfo_f( void );


/*
==============
VM_Init
==============
*/
void VM_Init( void ) {
	Cmd_AddCommand( "vminfo", VM_VmInfo_f );

	Com_Memset( vmTable, 0, sizeof( vmTable ) );
}


/*
================
VM_CreateHosted

A module the engine hosts itself. VM_Call reaches entry with the command and
up to 15 int32 arguments.
================
*/
vm_t *VM_CreateHosted( vmIndex_t index, vmMainFunc_t entry, void (*destroy)( vm_t *self ), vm_t *(*restart)( vm_t *self ) ) {
	vm_t *vm;

	if ( (unsigned)index >= VM_COUNT || !entry ) {
		Com_Error( ERR_FATAL, "VM_CreateHosted: bad parms" );
	}
	vm = &vmTable[ index ];
	if ( vm->name ) {
		VM_Free( vm );
	}
	vm->name = vmName[ index ];
	vm->index = index;
	vm->entryPoint = entry;
	vm->destroy = destroy;
	vm->restart = restart;
	vm->privateFlag = CVAR_PRIVATE;
	Q_strncpyz( vm->providerName, "qaa-recompiled", sizeof( vm->providerName ) );
	return vm;
}


/*
=================
VM_Restart

Hosted modules restart themselves.
=================
*/
vm_t *VM_Restart( vm_t *vm ) {
	if ( !vm->restart ) {
		Com_Printf( S_COLOR_RED "VM_Restart() failed: %s cannot restart\n", vm->name );
		return NULL;
	}
	return vm->restart( vm );
}


/*
=================
VM_Free
=================
*/
void VM_Free( vm_t *vm ) {

	if( !vm ) {
		return;
	}

	if ( vm->callLevel ) {
		if ( !forced_unload ) {
			Com_Error( ERR_FATAL, "VM_Free(%s) on running vm", vm->name );
			return;
		} else {
			Com_Printf( "forcefully unloading %s vm\n", vm->name );
		}
	}

	if ( vm->destroy )
		vm->destroy( vm );

	Com_Memset( vm, 0, sizeof( *vm ) );
}


void VM_Clear( void ) {
	int i;
	for ( i = 0; i < VM_COUNT; i++ ) {
		VM_Free( &vmTable[ i ] );
	}
}


void VM_Forced_Unload_Start(void) {
	forced_unload = 1;
}


void VM_Forced_Unload_Done(void) {
	forced_unload = 0;
}


/*
==============
VM_Call
==============
*/
intptr_t QDECL VM_Call( vm_t *vm, int nargs, int callnum, ... )
{
	int32_t args[MAX_VMMAIN_CALL_ARGS-1] = { 0 };
	va_list ap;
	intptr_t r;
	int i;

	if ( !vm || !vm->entryPoint ) {
		Com_Error( ERR_FATAL, "VM_Call with NULL vm" );
	}

	if ( nargs >= MAX_VMMAIN_CALL_ARGS ) {
		Com_Error( ERR_DROP, "VM_Call: nargs >= MAX_VMMAIN_CALL_ARGS" );
	}

	// reset syscall counter for top-level calls to detect infinite loops
	if ( vm->callLevel == 0 ) {
		vm->syscallCount = 0;
	}

	++vm->callLevel;

	va_start( ap, callnum );
	for ( i = 0; i < nargs; i++ ) {
		args[i] = va_arg( ap, int32_t );
	}
	va_end( ap );

	r = vm->entryPoint(
		callnum,
		args[0], args[1], args[2], args[3], args[4],
		args[5], args[6], args[7], args[8], args[9],
		args[10], args[11], args[12], args[13], args[14] );

	--vm->callLevel;

	return r;
}


/*
==============
VM_VmInfo_f
==============
*/
static void VM_VmInfo_f( void ) {
	const vm_t	*vm;
	int		i;

	Com_Printf( "Registered virtual machines:\n" );
	for ( i = 0 ; i < VM_COUNT ; i++ ) {
		vm = &vmTable[i];
		if ( !vm->name ) {
			continue;
		}
		Com_Printf( "%s : hosted (%s)\n", vm->name, vm->providerName );
	}
}
