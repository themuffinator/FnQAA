/*
 * openQAA: included into every recompiled module source through
 * XenonRecomp's ppc_config.h (compiled with -DPPC_INCLUDE_DETAIL).
 *
 * Replaces XenonRecomp's indirect-call lookup, which indexes a table
 * placed after a single image, with the runtime's two-level table shared
 * by every module and by host-call addresses, and hooks trap
 * instructions (the patched recompiler emits PPC_TRAP for them).
 */
#pragma once

#include <stdint.h>

struct PPCContext;

namespace qaa {
typedef void GuestFunc(PPCContext &__restrict__ ctx, uint8_t *base);
}

extern qaa::GuestFunc *const *const *qaa_module_func_pages;
extern void ( *qaa_module_indirect_miss )( PPCContext &ctx, uint8_t *base, uint32_t target );
extern void ( *qaa_module_trap )( PPCContext &ctx, uint8_t *base, uint32_t address );

static inline void qaa_call_indirect( PPCContext &ctx, uint8_t *base, uint32_t target ) {
	qaa::GuestFunc *const *page = qaa_module_func_pages[target >> 16];
	qaa::GuestFunc *fn = page ? page[( target & 0xFFFF ) >> 2] : nullptr;
	if ( fn ) {
		fn( ctx, base );
	} else {
		qaa_module_indirect_miss( ctx, base, target );
	}
}

#define PPC_CALL_INDIRECT_FUNC( x ) qaa_call_indirect( ctx, base, ( x ) )
#define PPC_TRAP( address ) qaa_module_trap( ctx, base, ( address ) )
