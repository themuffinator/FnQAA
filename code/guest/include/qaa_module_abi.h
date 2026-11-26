/*
 * openQAA guest runtime: the interface between the runtime and a
 * recompiled module library.
 *
 * A module library (qaa_qagame.dll, qaa_cgame.dll) is generated at build
 * time from the user's XEX by tools/recomp/build.py. It holds the
 * recompiled functions, the module's decoded image and a descriptor, and
 * exports one function, qaa_module_describe(). It links against nothing in
 * the runtime: everything it needs from the runtime (kernel calls, the
 * indirect-call table, trap reporting) arrives through qaa_module_host at
 * bind time, so one runtime can host several modules.
 *
 * This header is C++ only: PPCContext is XenonRecomp's guest CPU state.
 */
#ifndef QAA_MODULE_ABI_H
#define QAA_MODULE_ABI_H

#include <stddef.h>
#include <stdint.h>

#define QAA_MODULE_ABI_VERSION 1

struct PPCContext;

namespace qaa {

/* Same type as XenonRecomp's PPCFunc. */
typedef void GuestFunc(PPCContext &__restrict__ ctx, uint8_t *base);

/* Same layout as XenonRecomp's PPCFuncMapping. */
struct FuncMapping {
	size_t guest;
	GuestFunc *host;
};

enum ImportKind : uint32_t {
	IMPORT_FUNCTION = 0,
	IMPORT_VARIABLE = 1,
};

struct ModuleImport {
	const char *library;	/* "xboxkrnl.exe" */
	uint32_t ordinal;
	uint32_t kind;			/* ImportKind */
	uint32_t slot;			/* guest address of the import's address slot, or 0 */
	uint32_t thunk;			/* guest address of the 16-byte call thunk, or 0 */
	const char *name;
};

/*
 * Indirect calls: page[addr >> 16][(addr & 0xFFFF) >> 2]. A null page or
 * entry goes to indirect_miss, which also serves host-call addresses.
 */
enum { FUNC_PAGE_COUNT = 0x10000, FUNC_PAGE_ENTRIES = 0x4000 };

struct ModuleHost {
	uint32_t abi_version;
	void (*kernel_call)(uint32_t library, uint32_t ordinal, PPCContext &ctx, uint8_t *base);
	GuestFunc *const *const *func_pages;
	void (*indirect_miss)(PPCContext &ctx, uint8_t *base, uint32_t target);
	void (*trap)(PPCContext &ctx, uint8_t *base, uint32_t address);
};

struct ModuleDesc {
	uint32_t abi_version;
	uint32_t context_size;		/* sizeof(PPCContext) the module was built with */
	uint32_t config_flags;		/* QAA_PPC_CONFIG_* the module was built with */
	const char *name;			/* "qagame" */
	const char *original_name;	/* PE name from the XEX, "qagame.dll" */
	const char *recompiler;		/* XenonRecomp commit and patch set */
	uint32_t image_base;
	uint32_t image_size;
	uint32_t entry_point;
	uint32_t image_data_size;	/* stored bytes; the rest of the image is zero */
	const uint8_t *image_data;
	const FuncMapping *funcs;	/* terminated by { 0, nullptr } */
	const ModuleImport *imports;
	uint32_t import_count;
	const uint32_t *exports;	/* exports[ordinal - 1] */
	uint32_t export_count;
	void (*bind)(const ModuleHost *host);
};

/* XenonRecomp optimisation switches change PPCContext's layout. */
enum {
	QAA_PPC_CONFIG_SKIP_LR = 1 << 0,
	QAA_PPC_CONFIG_CTR_AS_LOCAL = 1 << 1,
	QAA_PPC_CONFIG_XER_AS_LOCAL = 1 << 2,
	QAA_PPC_CONFIG_RESERVED_AS_LOCAL = 1 << 3,
	QAA_PPC_CONFIG_SKIP_MSR = 1 << 4,
	QAA_PPC_CONFIG_CR_AS_LOCAL = 1 << 5,
	QAA_PPC_CONFIG_NON_ARGUMENT_AS_LOCAL = 1 << 6,
	QAA_PPC_CONFIG_NON_VOLATILE_AS_LOCAL = 1 << 7,
};

/* The flags of the ppc_config.h included before this header. */
static inline uint32_t PpcConfigFlags() {
	uint32_t f = 0;
#ifdef PPC_CONFIG_SKIP_LR
	f |= QAA_PPC_CONFIG_SKIP_LR;
#endif
#ifdef PPC_CONFIG_CTR_AS_LOCAL
	f |= QAA_PPC_CONFIG_CTR_AS_LOCAL;
#endif
#ifdef PPC_CONFIG_XER_AS_LOCAL
	f |= QAA_PPC_CONFIG_XER_AS_LOCAL;
#endif
#ifdef PPC_CONFIG_RESERVED_AS_LOCAL
	f |= QAA_PPC_CONFIG_RESERVED_AS_LOCAL;
#endif
#ifdef PPC_CONFIG_SKIP_MSR
	f |= QAA_PPC_CONFIG_SKIP_MSR;
#endif
#ifdef PPC_CONFIG_CR_AS_LOCAL
	f |= QAA_PPC_CONFIG_CR_AS_LOCAL;
#endif
#ifdef PPC_CONFIG_NON_ARGUMENT_AS_LOCAL
	f |= QAA_PPC_CONFIG_NON_ARGUMENT_AS_LOCAL;
#endif
#ifdef PPC_CONFIG_NON_VOLATILE_AS_LOCAL
	f |= QAA_PPC_CONFIG_NON_VOLATILE_AS_LOCAL;
#endif
	return f;
}

} // namespace qaa

typedef const qaa::ModuleDesc *( *qaa_module_describe_fn )( void );
#define QAA_MODULE_DESCRIBE_SYMBOL "qaa_module_describe"

#endif
