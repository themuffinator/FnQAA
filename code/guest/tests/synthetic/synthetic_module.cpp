/*
 * A synthetic guest module for the runtime's unit tests: hand-written
 * functions in the shape XenonRecomp emits (PPC_FUNC bodies working on
 * PPCContext and big-endian guest memory), a small made-up image, and the
 * same descriptor and glue a recompiled module has. No retail code or data.
 *
 * Image (base 0x82000000, size 0x20000):
 *   0x82000400  import slot: XboxHardwareInfo (variable)
 *   0x82000404+ import slots: functions (receive their thunk addresses)
 *   0x82001000  "hello"            0x82001010  "game:\test.txt"
 *   0x82001100  function pointer table: { 0x82010300 }
 *   0x82002000  syscall pointer (dllEntry)  0x82002004  TLS index
 *   0x82002008  processor count             0x8200200C  thread result
 *   0x82002010  event handle   0x82002020  critical section
 *   0x82002040  read buffer    0x82002100  scratch
 *   0x82010000  entry point    0x82010100  dllEntry   0x82010200  vmMain
 *   0x82010300  add(r3, r4)    0x82010400  thread start
 *   0x82010F00+ import thunks (16 bytes each)
 */
#include "ppc_config.h"
#include <ppc_context.h>

#include "qaa_module_abi.h"

#include <cstring>

#ifdef _WIN32
#define QAA_MODULE_EXPORT extern "C" __declspec(dllexport)
#else
#define QAA_MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

qaa::GuestFunc *const *const *qaa_module_func_pages;
void (*qaa_module_indirect_miss)(PPCContext &ctx, uint8_t *base, uint32_t target);
void (*qaa_module_trap)(PPCContext &ctx, uint8_t *base, uint32_t address);
static void (*qaa_kernel_call)(uint32_t library, uint32_t ordinal, PPCContext &ctx, uint8_t *base);

#define KERNEL_THUNKS(X) \
	X(0, KeTlsAlloc, 0x152) \
	X(1, KeTlsGetValue, 0x154) \
	X(2, KeTlsSetValue, 0x155) \
	X(3, NtAllocateVirtualMemory, 0x0CC) \
	X(4, RtlInitializeCriticalSection, 0x12E) \
	X(5, RtlEnterCriticalSection, 0x125) \
	X(6, RtlLeaveCriticalSection, 0x130) \
	X(7, NtCreateEvent, 0x0D1) \
	X(8, NtSetEvent, 0x0F6) \
	X(9, NtWaitForSingleObjectEx, 0x0FD) \
	X(10, ExCreateThread, 0x00D) \
	X(11, RtlInitAnsiString, 0x12C) \
	X(12, NtOpenFile, 0x0DF) \
	X(13, NtReadFile, 0x0F0) \
	X(14, NtClose, 0x0CF)

#define DEFINE_THUNK(i, name, ord) \
	PPC_FUNC(__imp__##name) { qaa_kernel_call(0, ord, ctx, base); }
KERNEL_THUNKS(DEFINE_THUNK)

static uint32_t Thunk(int i) { return 0x82010F00 + 16 * i; }

/* Make a stack frame of 0x80 bytes (back chain, 0x50 linkage, room for args). */
struct Frame {
	PPCContext &ctx;
	uint8_t *base;
	uint32_t sp;
	Frame(PPCContext &c, uint8_t *b) : ctx(c), base(b), sp(c.r1.u32) {
		ctx.r1.u64 = sp - 0x80;
		PPC_STORE_U32(ctx.r1.u32, sp);
	}
	~Frame() { ctx.r1.u64 = sp; }
};

static void Syscall(PPCContext &ctx, uint8_t *base) {
	uint32_t fn = PPC_LOAD_U32(0x82002000);
	ctx.lr = 0x82010280;
	PPC_CALL_INDIRECT_FUNC(fn);
}

/* entry point: DllMain(hModule, reason, reserved) */
PPC_FUNC(sub_82010000) {
	if (ctx.r4.u32 != 1) {	/* only DLL_PROCESS_ATTACH does work */
		ctx.r3.u64 = 1;
		return;
	}
	Frame f(ctx, base);
	__imp__KeTlsAlloc(ctx, base);
	PPC_STORE_U32(0x82002004, ctx.r3.u32);
	uint32_t hw = PPC_LOAD_U32(0x82000400);
	PPC_STORE_U32(0x82002008, PPC_LOAD_U8(hw + 4));
	ctx.r3.u64 = 1;
}

/* dllEntry(syscall) */
PPC_FUNC(sub_82010100) {
	PPC_STORE_U32(0x82002000, ctx.r3.u32);
}

/* add(a, b) */
PPC_FUNC(sub_82010300) {
	ctx.r3.u64 = ctx.r3.u64 + ctx.r4.u64;
}

/* thread start(context) */
PPC_FUNC(sub_82010400) {
	Frame f(ctx, base);
	PPC_STORE_U32(0x8200200C, 0x1200 + ctx.r3.u32);
	ctx.r3.u64 = PPC_LOAD_U32(0x82002010);
	ctx.r4.u64 = 0;
	__imp__NtSetEvent(ctx, base);
	ctx.r3.u64 = 0;
}

/* vmMain(command, arg0, arg1, ...) */
PPC_FUNC(sub_82010200) {
	Frame f(ctx, base);
	const uint32_t sp = ctx.r1.u32;
	switch (ctx.r3.u32) {
	case 0: {	/* a syscall with nine arguments, two on the stack, and a float */
		ctx.r3.s64 = 100;
		ctx.r4.u64 = 0x82001000;
		ctx.r5.s64 = 2;
		ctx.r6.s64 = 3;
		ctx.r7.s64 = 4;
		ctx.r8.s64 = 5;
		ctx.r9.s64 = -6;
		ctx.r10.s64 = 7;
		PPC_STORE_U64(sp + 0x50, 8);
		PPC_STORE_U64(sp + 0x58, 9);
		ctx.f1.f64 = 1.5;
		Syscall(ctx, base);
		ctx.r3.s64 += 1;
		return;
	}
	case 1: {	/* NtAllocateVirtualMemory, then touch the last word */
		PPC_STORE_U32(0x82002100, 0);
		PPC_STORE_U32(0x82002104, 0x30000);
		ctx.r3.u64 = 0x82002100;
		ctx.r4.u64 = 0x82002104;
		ctx.r5.u64 = 0x3000;	/* MEM_COMMIT | MEM_RESERVE */
		ctx.r6.u64 = 4;
		ctx.r7.u64 = 0;
		__imp__NtAllocateVirtualMemory(ctx, base);
		if (ctx.r3.u32) {
			ctx.r3.s64 = -1;
			return;
		}
		uint32_t a = PPC_LOAD_U32(0x82002100);
		PPC_STORE_U32(a + 0x2FFFC, 0xCAFEBABE);
		ctx.r3.u64 = PPC_LOAD_U32(a + 0x2FFFC) == 0xCAFEBABE ? a : 0;
		return;
	}
	case 2: {	/* indirect call through a function pointer table */
		uint32_t fn = PPC_LOAD_U32(0x82001100);
		ctx.r3.u64 = ctx.r4.u64;
		ctx.r4.u64 = ctx.r5.u64;
		PPC_CALL_INDIRECT_FUNC(fn);
		return;
	}
	case 3:		/* read an unmapped address */
		ctx.r3.u64 = PPC_LOAD_U32(0x10);
		return;
	case 4:		/* a trap instruction */
		PPC_TRAP(0x82010244);
		return;
	case 5: {	/* critical section: enter twice, read the recursion count, leave twice */
		ctx.r3.u64 = 0x82002020;
		__imp__RtlInitializeCriticalSection(ctx, base);
		for (int i = 0; i < 2; i++) {
			ctx.r3.u64 = 0x82002020;
			__imp__RtlEnterCriticalSection(ctx, base);
		}
		uint32_t rec = PPC_LOAD_U32(0x82002020 + 0x14);
		for (int i = 0; i < 2; i++) {
			ctx.r3.u64 = 0x82002020;
			__imp__RtlLeaveCriticalSection(ctx, base);
		}
		ctx.r3.u64 = rec * 10 + PPC_LOAD_U32(0x82002020 + 0x18);
		return;
	}
	case 6:		/* a syscall during which the host calls vmMain again */
		ctx.r3.s64 = 200;
		Syscall(ctx, base);
		return;
	case 7: {	/* a thread that signals an event */
		PPC_STORE_U32(0x8200200C, 0);
		ctx.r3.u64 = 0x82002010;
		ctx.r4.u64 = 0;
		ctx.r5.u64 = 1;		/* SynchronizationEvent */
		ctx.r6.u64 = 0;
		__imp__NtCreateEvent(ctx, base);
		ctx.r3.u64 = 0x82002014;
		ctx.r4.u64 = 0;
		ctx.r5.u64 = 0;
		ctx.r6.u64 = 0;
		ctx.r7.u64 = 0x82010400;
		ctx.r8.u64 = 0x55;
		ctx.r9.u64 = 0;
		__imp__ExCreateThread(ctx, base);
		ctx.r3.u64 = PPC_LOAD_U32(0x82002010);
		ctx.r4.u64 = 1;
		ctx.r5.u64 = 0;
		ctx.r6.u64 = 0;
		__imp__NtWaitForSingleObjectEx(ctx, base);
		ctx.r3.u64 = PPC_LOAD_U32(0x8200200C);
		return;
	}
	case 8: {	/* TLS round trip */
		uint32_t idx = PPC_LOAD_U32(0x82002004);
		ctx.r3.u64 = idx;
		ctx.r4.u64 = 0xABCD;
		__imp__KeTlsSetValue(ctx, base);
		ctx.r3.u64 = idx;
		__imp__KeTlsGetValue(ctx, base);
		return;
	}
	case 9: {	/* open and read a file through the host */
		ctx.r3.u64 = 0x82001040;
		ctx.r4.u64 = 0x82001010;
		__imp__RtlInitAnsiString(ctx, base);
		PPC_STORE_U32(0x82001050, 0);
		PPC_STORE_U32(0x82001054, 0x82001040);
		PPC_STORE_U32(0x82001058, 0);
		ctx.r3.u64 = 0x82002110;
		ctx.r4.u64 = 0x80000000;	/* GENERIC_READ */
		ctx.r5.u64 = 0x82001050;
		ctx.r6.u64 = 0x82002118;
		ctx.r7.u64 = 1;
		ctx.r8.u64 = 0;
		__imp__NtOpenFile(ctx, base);
		if (ctx.r3.u32) {
			ctx.r3.s64 = -2;
			return;
		}
		uint32_t h = PPC_LOAD_U32(0x82002110);
		ctx.r3.u64 = h;
		ctx.r4.u64 = 0;
		ctx.r5.u64 = 0;
		ctx.r6.u64 = 0;
		ctx.r7.u64 = 0x82002118;
		ctx.r8.u64 = 0x82002040;
		ctx.r9.u64 = 64;
		ctx.r10.u64 = 0;
		__imp__NtReadFile(ctx, base);
		uint32_t n = PPC_LOAD_U32(0x82002118 + 4);
		ctx.r3.u64 = h;
		__imp__NtClose(ctx, base);
		ctx.r3.u64 = n;
		return;
	}
	case 10:	/* an indirect call to an address with no function */
		PPC_CALL_INDIRECT_FUNC(0x82012000);
		return;
	case 11:	/* a syscall the host aborts */
		ctx.r3.s64 = 300;
		Syscall(ctx, base);
		ctx.r3.s64 = 0x5555;	/* never reached */
		return;
	default:
		ctx.r3.s64 = -1;
		return;
	}
}

#define MAP_THUNK(i, name, ord) { 0x82010F00 + 16 * i, __imp__##name },
static const qaa::FuncMapping funcs[] = {
	{ 0x82010000, sub_82010000 },
	{ 0x82010100, sub_82010100 },
	{ 0x82010200, sub_82010200 },
	{ 0x82010300, sub_82010300 },
	{ 0x82010400, sub_82010400 },
	KERNEL_THUNKS(MAP_THUNK)
	{ 0, nullptr },
};

#define IMPORT_ENTRY(i, name, ord) { "xboxkrnl.exe", ord, qaa::IMPORT_FUNCTION, 0x82000404u + 4 * i, 0x82010F00u + 16 * i, #name },
static const qaa::ModuleImport imports[] = {
	{ "xboxkrnl.exe", 0x156, qaa::IMPORT_VARIABLE, 0x82000400, 0, "XboxHardwareInfo" },
	KERNEL_THUNKS(IMPORT_ENTRY)
};

static const uint32_t exports[] = { 0x82010200, 0x82010100 };

static uint8_t image[0x3000];

static void Put(uint32_t a, const char *s) { memcpy(image + (a - 0x82000000), s, strlen(s) + 1); }
static void Put32(uint32_t a, uint32_t v) {
	v = __builtin_bswap32(v);
	memcpy(image + (a - 0x82000000), &v, 4);
}

static void Bind(const qaa::ModuleHost *host) {
	qaa_module_func_pages = host->func_pages;
	qaa_module_indirect_miss = host->indirect_miss;
	qaa_module_trap = host->trap;
	qaa_kernel_call = host->kernel_call;
}

QAA_MODULE_EXPORT const qaa::ModuleDesc *qaa_module_describe(void) {
	static const qaa::ModuleDesc desc = [] {
		Put(0x82001000, "hello");
		Put(0x82001010, "game:\\test.txt");
		Put32(0x82001100, 0x82010300);
		qaa::ModuleDesc d = {};
		d.abi_version = QAA_MODULE_ABI_VERSION;
		d.context_size = sizeof(PPCContext);
		d.config_flags = qaa::PpcConfigFlags();
		d.name = "synthetic";
		d.original_name = "synthetic.dll";
		d.recompiler = "hand-written test module";
		d.image_base = 0x82000000;
		d.image_size = 0x20000;
		d.entry_point = 0x82010000;
		d.image_data_size = sizeof(image);
		d.image_data = image;
		d.funcs = funcs;
		d.imports = imports;
		d.import_count = sizeof(imports) / sizeof(imports[0]);
		d.exports = exports;
		d.export_count = 2;
		d.bind = Bind;
		return d;
	}();
	return &desc;
}
