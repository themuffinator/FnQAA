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
 *   0x82002040  read buffer    0x820020C0  hModule   0x82002100  scratch
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
	X(14, NtClose, 0x0CF) \
	X(15, RtlImageXexHeaderField, 0x12B) \
	X(16, NtQueryDirectoryFile, 0x0E4) \
	X(17, RtlRaiseException, 0x136) \
	X(18, NtSetInformationFile, 0x0F7)

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
	PPC_STORE_U32(0x820020C0, ctx.r3.u32);	/* hModule */
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

/* ---- SEH: outer { __try { mid() } __except (code == NO_MEMORY) },
 *          mid { save r31; __try { raiser() } __finally { count } },
 *          raiser(mode): 0 returns 5, 1 raises NO_MEMORY, 2 raises E0001234.
 * Written as the recompiler emits it: frames with back chain and saved LR,
 * PPC_SEH_RESUME checks after the calls at the SEH sites. */
uint32_t (*qaa_module_seh_resume)(PPCContext &ctx, uint8_t *base, uint32_t site);

static void Prologue(PPCContext &ctx, uint8_t *base, uint32_t frame, bool save_r31) {
	PPC_STORE_U32(ctx.r1.u32 - 8, (uint32_t)ctx.lr);
	if (save_r31)
		PPC_STORE_U64(ctx.r1.u32 - 0x10, ctx.r31.u64);
	uint32_t caller = ctx.r1.u32;
	ctx.r1.u64 = caller - frame;
	PPC_STORE_U32(ctx.r1.u32, caller);
}

static void Epilogue(PPCContext &ctx, uint8_t *base, bool restore_r31) {
	uint32_t caller = PPC_LOAD_U32(ctx.r1.u32);
	if (restore_r31)
		ctx.r31.u64 = PPC_LOAD_U64(caller - 0x10);
	ctx.r1.u64 = caller;
}

PPC_FUNC(sub_82010680) {	/* raiser(mode) */
	Prologue(ctx, base, 0x60, false);
	if (ctx.r3.u32 == 0) {
		ctx.r3.u64 = 5;
		Epilogue(ctx, base, false);
		return;
	}
	PPC_STORE_U32(0x82002300, ctx.r3.u32 == 1 ? 0xC0000017 : 0xE0001234);
	PPC_STORE_U32(0x82002300 + 4, 0);
	ctx.r3.u64 = 0x82002300;
	ctx.lr = 0x82010694;
	__imp__RtlRaiseException(ctx, base);	/* site 0x82010690 */
	{
		const uint32_t a = PPC_SEH_RESUME(0x82010690);
		if (a == 1)
			return;
	}
	ctx.r3.s64 = -55;	/* a raise returned */
	Epilogue(ctx, base, false);
}

PPC_FUNC(sub_82010600) {	/* mid(mode) */
	Prologue(ctx, base, 0x80, true);
	ctx.r31.u64 = 0x44444444;
	ctx.lr = 0x82010614;
	sub_82010680(ctx, base);	/* site 0x82010610, in a __finally scope */
	{
		const uint32_t a = PPC_SEH_RESUME(0x82010610);
		if (a == 1)
			return;
	}
	ctx.r3.u64 += 100;
	Epilogue(ctx, base, true);
}

PPC_FUNC(sub_82010700) {	/* mid's __finally funclet: r12 = mid's caller SP */
	PPC_STORE_U32(0x82002304, PPC_LOAD_U32(0x82002304) + 1);
	PPC_STORE_U32(0x82002308, ctx.r12.u32);
	PPC_STORE_U32(0x8200230C, ctx.r3.u32);	/* abnormal termination flag */
}

PPC_FUNC(sub_82010720) {	/* outer's filter: EXECUTE_HANDLER for STATUS_NO_MEMORY */
	ctx.r3.u64 = PPC_LOAD_U32(PPC_LOAD_U32(ctx.r3.u32)) == 0xC0000017 ? 1 : 0;
}

PPC_FUNC(sub_82010500) {	/* outer(mode) */
	Prologue(ctx, base, 0x80, true);
	ctx.r31.u64 = 0x31313131;
	PPC_STORE_U32(0x82002310, ctx.r1.u32);	/* mid's caller SP, for the test */
	ctx.lr = 0x82010514;
	sub_82010600(ctx, base);	/* site 0x82010510, in the __except scope */
	{
		const uint32_t a = PPC_SEH_RESUME(0x82010510);
		if (a == 1)
			return;
		if (a == 2)
			goto loc_82010530;
	}
	ctx.r3.u64 = ctx.r31.u64 == 0x31313131 ? ctx.r3.u64 + 1000 : (uint64_t)-9;
	goto loc_82010540;
loc_82010530:	/* __except handler */
	ctx.r3.u64 = ctx.r31.u64 == 0x31313131 ? 7000 : (uint64_t)-7;
loc_82010540:
	Epilogue(ctx, base, true);
}

static const qaa::SehScope seh_scopes[] = {
	{ 0x82010510, 0x82010520, 0x82010720, 0x82010530 },	/* outer: __except */
	{ 0x82010610, 0x82010620, 0x82010700, 0 },			/* mid: __finally */
};
static const qaa::SehFunction seh_functions[] = {
	{ 0x82010500, 0x82010600, 0, 1 },
	{ 0x82010600, 0x82010680, 1, 1 },
};
static const qaa::SehSite seh_sites[] = {
	{ 0x82010510, 0x82010530 },
	{ 0x82010610, 0 },
	{ 0x82010690, 0 },
};
static const qaa::UnwindSave unwind_saves[] = {
	{ qaa::UNWIND_GPR, 31, -0x10 },
};
static const qaa::UnwindFunction unwind_functions[] = {
	{ 0x82010600, 0x82010680, 0, 1 },	/* mid */
	{ 0x82010680, 0x82010700, 1, 0 },	/* raiser */
};

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
	case 12:	/* the module's own XEX header: default heap size (0x20401) */
	case 13: {	/* the executable's, through XexExecutableModuleHandle */
		uint32_t ldr = ctx.r3.u32 == 12 ? PPC_LOAD_U32(0x820020C0) : PPC_LOAD_U32(PPC_LOAD_U32(0x82000480));
		ctx.r3.u64 = PPC_LOAD_U32(ldr + 0x58);
		ctx.r4.u64 = 0x20401;
		__imp__RtlImageXexHeaderField(ctx, base);
		ctx.r3.u64 = ctx.r3.u32 ? PPC_LOAD_U32(ctx.r3.u32) : 0;
		return;
	}
	case 14: {	/* list "game:\dir" matching "*.txt"; return count * 100 + last name length */
		ctx.r3.u64 = 0x82001040;
		ctx.r4.u64 = 0x82001030;
		__imp__RtlInitAnsiString(ctx, base);
		PPC_STORE_U32(0x82001050, 0);
		PPC_STORE_U32(0x82001054, 0x82001040);
		PPC_STORE_U32(0x82001058, 0);
		ctx.r3.u64 = 0x82002110;
		ctx.r4.u64 = 0x80000000;
		ctx.r5.u64 = 0x82001050;
		ctx.r6.u64 = 0x82002118;
		ctx.r7.u64 = 1;
		ctx.r8.u64 = 1;		/* FILE_DIRECTORY_FILE */
		__imp__NtOpenFile(ctx, base);
		if (ctx.r3.u32) {
			ctx.r3.s64 = -2;
			return;
		}
		uint32_t h = PPC_LOAD_U32(0x82002110);
		ctx.r3.u64 = 0x82001060;	/* the pattern as an ANSI_STRING */
		ctx.r4.u64 = 0x82001038;
		__imp__RtlInitAnsiString(ctx, base);
		uint32_t count = 0, last = 0;
		for (;;) {
			ctx.r3.u64 = h;
			ctx.r4.u64 = 0;
			ctx.r5.u64 = 0;
			ctx.r6.u64 = 0;
			ctx.r7.u64 = 0x82002118;
			ctx.r8.u64 = 0x82002200;
			ctx.r9.u64 = 0x200;
			ctx.r10.u64 = 0x82001060;
			PPC_STORE_U64(sp + 0x50, 0);	/* restart_scan */
			__imp__NtQueryDirectoryFile(ctx, base);
			if (ctx.r3.u32)
				break;
			count++;
			last = PPC_LOAD_U32(0x82002200 + 0x3C);
		}
		ctx.r3.u64 = h;
		__imp__NtClose(ctx, base);
		ctx.r3.u64 = count * 100 + last;
		return;
	}
	case 16: {	/* open game:\test.txt, set its end of file to 7: the status */
		ctx.r3.u64 = 0x82001040;
		ctx.r4.u64 = 0x82001010;
		__imp__RtlInitAnsiString(ctx, base);
		PPC_STORE_U32(0x82001050, 0);
		PPC_STORE_U32(0x82001054, 0x82001040);
		PPC_STORE_U32(0x82001058, 0);
		ctx.r3.u64 = 0x82002110;
		ctx.r4.u64 = 0x80000000;
		ctx.r5.u64 = 0x82001050;
		ctx.r6.u64 = 0x82002118;
		ctx.r7.u64 = 1;
		ctx.r8.u64 = 0;
		__imp__NtOpenFile(ctx, base);
		uint32_t h = PPC_LOAD_U32(0x82002110);
		PPC_STORE_U64(0x82002120, 7);
		ctx.r3.u64 = h;
		ctx.r4.u64 = 0x82002118;
		ctx.r5.u64 = 0x82002120;
		ctx.r6.u64 = 8;
		ctx.r7.u64 = 20;	/* FileEndOfFileInformation */
		__imp__NtSetInformationFile(ctx, base);
		uint32_t status = ctx.r3.u32;
		ctx.r3.u64 = h;
		__imp__NtClose(ctx, base);
		ctx.r3.u64 = status;
		return;
	}
	case 15:	/* SEH: outer(arg0) */
		ctx.r3.u64 = ctx.r4.u64;
		ctx.lr = 0x82010284;
		sub_82010500(ctx, base);
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
	{ 0x82010500, sub_82010500 },
	{ 0x82010600, sub_82010600 },
	{ 0x82010680, sub_82010680 },
	{ 0x82010700, sub_82010700 },
	{ 0x82010720, sub_82010720 },
	KERNEL_THUNKS(MAP_THUNK)
	{ 0, nullptr },
};

#define IMPORT_ENTRY(i, name, ord) { "xboxkrnl.exe", ord, qaa::IMPORT_FUNCTION, 0x82000404u + 4 * i, 0x82010F00u + 16 * i, #name },
static const qaa::ModuleImport imports[] = {
	{ "xboxkrnl.exe", 0x156, qaa::IMPORT_VARIABLE, 0x82000400, 0, "XboxHardwareInfo" },
	{ "xboxkrnl.exe", 0x193, qaa::IMPORT_VARIABLE, 0x82000480, 0, "XexExecutableModuleHandle" },
	KERNEL_THUNKS(IMPORT_ENTRY)
};

static const uint32_t exports[] = { 0x82010200, 0x82010100 };

static uint8_t image[0x3000];

/* A made-up XEX header: magic, then one optional header, 0x20401 (default
 * heap size, inline value) = 0x00200000. Headers start at +0x18. */
static const uint8_t xex_header[0x20] = {
	'X', 'E', 'X', '2', 0, 0, 0, 0, 0, 0, 0, 0x20, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 1, 0, 0x02, 0x04, 0x01, 0x00, 0x20, 0x00, 0x00,
};

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
	qaa_module_seh_resume = host->seh_resume;
}

QAA_MODULE_EXPORT const qaa::ModuleDesc *qaa_module_describe(void) {
	static const qaa::ModuleDesc desc = [] {
		Put(0x82001000, "hello");
		Put(0x82001010, "game:\\test.txt");
		Put(0x82001030, "game:\\d");
		Put(0x82001038, "*.txt");
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
		d.xex_header = xex_header;
		d.xex_header_size = sizeof(xex_header);
		d.seh_functions = seh_functions;
		d.seh_function_count = 2;
		d.seh_scopes = seh_scopes;
		d.seh_sites = seh_sites;
		d.seh_site_count = 3;
		d.unwind_functions = unwind_functions;
		d.unwind_function_count = 2;
		d.unwind_saves = unwind_saves;
		return d;
	}();
	return &desc;
}
