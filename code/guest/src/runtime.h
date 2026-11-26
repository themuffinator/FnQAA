/*
 * FnQAA guest runtime: internal declarations shared by guest.cpp and
 * kernel.cpp.
 */
#pragma once

#include "ppc_config.h"
#include <ppc_context.h>

#include "qaa_guest.h"
#include "qaa_module_abi.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace qaa {

/* ---- big-endian guest memory access ------------------------------------- */

inline uint32_t Bswap32(uint32_t v) { return __builtin_bswap32(v); }
inline uint16_t Bswap16(uint16_t v) { return __builtin_bswap16(v); }
inline uint64_t Bswap64(uint64_t v) { return __builtin_bswap64(v); }

struct Memory {
	uint8_t *base = nullptr;			/* host address of guest 0 */
	std::vector<uint8_t> committed;		/* one byte per 4 KiB guest page */
	std::mutex lock;

	bool Reserve();
	void Release();
	bool Commit(uint32_t address, uint32_t size);
	void Decommit(uint32_t address, uint32_t size);
	bool IsCommitted(uint32_t address, uint32_t size) const;

	uint8_t *Ptr(uint32_t a) const { return base + a; }
	uint8_t U8(uint32_t a) const { return base[a]; }
	uint16_t U16(uint32_t a) const { uint16_t v; memcpy(&v, base + a, 2); return Bswap16(v); }
	uint32_t U32(uint32_t a) const { uint32_t v; memcpy(&v, base + a, 4); return Bswap32(v); }
	uint64_t U64(uint32_t a) const { uint64_t v; memcpy(&v, base + a, 8); return Bswap64(v); }
	void W8(uint32_t a, uint8_t v) { base[a] = v; }
	void W16(uint32_t a, uint16_t v) { v = Bswap16(v); memcpy(base + a, &v, 2); }
	void W32(uint32_t a, uint32_t v) { v = Bswap32(v); memcpy(base + a, &v, 4); }
	void W64(uint32_t a, uint64_t v) { v = Bswap64(v); memcpy(base + a, &v, 8); }
	std::string String(uint32_t a, size_t max = 4096) const;
};

/* First-fit allocator over a guest range, for kernel objects and the
 * host's qaa_guest_alloc. 16-byte granularity. */
struct Pool {
	uint32_t begin = 0, end = 0, top = 0, committed_top = 0;
	std::map<uint32_t, uint32_t> free_blocks;	/* address -> size */
	std::map<uint32_t, uint32_t> used;			/* address -> size */
	std::mutex lock;
	Memory *memory = nullptr;

	void Init(Memory *m, uint32_t b, uint32_t e) { memory = m; begin = top = committed_top = b; end = e; }
	uint32_t Alloc(uint32_t size, uint32_t align = 16);
	void Free(uint32_t address);
};

/* NtAllocateVirtualMemory's arena: 64 KiB reservations, 4 KiB commits. */
struct VirtualArena {
	enum : uint8_t { FREE, RESERVED, COMMITTED };
	uint32_t begin = 0, end = 0;
	std::vector<uint8_t> pages;				/* per 4 KiB page */
	std::map<uint32_t, uint32_t> regions;	/* allocation base -> reserved size */
	std::mutex lock;
	Memory *memory = nullptr;

	void Init(Memory *m, uint32_t b, uint32_t e);
	/* NT semantics; returns an NTSTATUS. */
	uint32_t Allocate(uint32_t *address, uint32_t *size, uint32_t type, bool top_down);
	uint32_t Free(uint32_t *address, uint32_t *size, uint32_t type);
	bool Query(uint32_t address, uint32_t out[7]);
	bool Owns(uint32_t a) const { return a >= begin && a < end; }
};

/* ---- threads -------------------------------------------------------------- */

struct Guest;
struct KObject;

enum { TLS_SLOTS = 64 };

struct Thread {
	Guest *guest = nullptr;
	PPCContext *ctx = nullptr;
	uint32_t stack_base = 0, stack_size = 0;	/* guest; the stack grows down from base + size */
	uint32_t pcr = 0, kthread = 0;
	uint32_t thread_id = 0;
	uint32_t tls[TLS_SLOTS] = {};
	int depth = 0;								/* nested calls into the guest */
	const struct KObject *blocked_on = nullptr;	/* first object of the wait this thread is in (wait_lock) */
	uint64_t waits_begun = 0;					/* waits entered so far (wait_lock) */
	struct {									/* a guest exception being unwound */
		bool active = false;
		int frames_left = 0;					/* frames still to leave before the handler */
		uint32_t target = 0;					/* the __except handler label */
	} seh;
	bool guest_created = false;
	std::shared_ptr<KObject> object;			/* for guest-created threads */

	~Thread();
};

/* ---- kernel objects --------------------------------------------------------- */

struct KObject {
	enum Type { K_EVENT, K_THREAD, K_FILE } type;
	explicit KObject(Type t) : type(t) {}
	virtual ~KObject() = default;

	/* waitable state, guarded by Guest::wait_lock */
	bool signaled = false;
	bool manual_reset = true;
};

struct KThreadObject : KObject {
	KThreadObject() : KObject(K_THREAD) {}
	Thread *thread = nullptr;	/* runs on a detached host thread (StartGuestThread) */
	uint32_t kthread = 0;
	uint32_t exit_code = 0;
	uint32_t start_address = 0, start_context = 0, xapi_startup = 0;
	uint32_t suspend_count = 0;

};

struct KFileObject : KObject {
	KFileObject() : KObject(K_FILE) {}
	int64_t host_handle = -1;
	int64_t position = 0;
	std::string path;
	bool is_directory = false;
	uint32_t dir_index = 0;		/* NtQueryDirectoryFile position */
	std::string dir_pattern;
};

/* ---- modules ---------------------------------------------------------------- */

struct HostCall {
	qaa_syscall_fn fn = nullptr;
	void *user = nullptr;
	qaa_module *module = nullptr;
};

struct Hook {
	qaa_hook_fn fn = nullptr;
	void *user = nullptr;
};

} // namespace qaa

struct qaa_module {
	qaa::Guest *guest = nullptr;
	void *library = nullptr;
	const qaa::ModuleDesc *desc = nullptr;
	std::string name;
	uint32_t ldr_entry = 0;			/* fake LDR_DATA_TABLE_ENTRY passed as hModule */
	uint32_t syscall_address = 0;	/* host-call address given to dllEntry */
};

namespace qaa {

struct Guest {
	qaa_host host = {};
	Memory memory;
	Pool pool;				/* kernel objects, HLE data, qaa_guest_alloc */
	VirtualArena arena;		/* NtAllocateVirtualMemory */
	uint32_t next_stack = QAA_GUEST_STACK_BASE;
	std::mutex stack_lock;

	GuestFunc **pages[FUNC_PAGE_COUNT] = {};
	ModuleHost module_host = {};

	std::vector<HostCall> host_calls;	/* reserved up front: entries never move */
	std::mutex host_call_lock;
	std::vector<std::unique_ptr<qaa_module>> modules;

	/* host implementations of guest functions and imports (qaa_guest_set_hook) */
	std::mutex hook_lock;
	std::unordered_map<uint32_t, Hook> hooks;
	Hook default_hook;
	std::unordered_map<uint32_t, Hook> import_hooks;	/* library << 16 | ordinal */
	Hook default_import_hook;

	/* kernel state */
	std::mutex handle_lock;
	std::unordered_map<uint32_t, std::shared_ptr<KObject>> handles;
	uint32_t next_handle = 0x1000;
	std::atomic<uint32_t> next_thread_id{ 1 };
	std::mutex tls_lock;
	uint64_t tls_used = 0;
	std::mutex cs_lock;	/* critical-section wait/wake */
	std::condition_variable cs_cv;
	std::mutex wait_lock;	/* every waitable object's state, thread suspension */
	std::condition_variable wait_cv;
	bool trace_kernel = false;
	uint32_t var_hardware_info = 0, var_debug_monitor = 0, var_thread_type = 0;
	uint32_t var_exe_module = 0, exe_ldr_entry = 0, exe_xex_header = 0;
	uint32_t var_time_stamp = 0, var_event_type = 0, var_krnl_version = 0;
	std::map<uint32_t, uint32_t> var_blank;	/* imports with no meaning on the PC: zeroed blocks */
	std::mutex var_lock;
	std::chrono::steady_clock::time_point start_time;
	double tsc_frequency = 0;

	std::mutex error_lock;
	std::string last_error;

	std::mutex threads_lock;
	std::vector<std::shared_ptr<KThreadObject>> threads;

	void Log(int level, const char *fmt, ...);
	void Logv(int level, const char *fmt, va_list ap);
	void SetError(const std::string &e);

	GuestFunc *Lookup(uint32_t address) const {
		GuestFunc *const *page = pages[address >> 16];
		return page ? page[(address & 0xFFFF) >> 2] : nullptr;
	}
	void Register(uint32_t address, GuestFunc *fn);

	uint32_t AddHandle(std::shared_ptr<KObject> obj);
	std::shared_ptr<KObject> GetHandle(uint32_t handle);
	bool CloseHandle(uint32_t handle);

	Thread *CurrentThread();		/* creates the host thread's guest state */
	Thread *NewThread(uint32_t stack_size);
	uint32_t AllocStack(uint32_t size);
};

extern thread_local Thread *tls_current_thread;

/* Abandon the current call into the guest with an error (does not return). */
[[noreturn]] void Fatal(const char *fmt, ...);
/* Leave the current guest thread (ExTerminateThread). */
[[noreturn]] void ExitThread(uint32_t code);

/* Run fn(ctx, base) with faults caught; returns 0, or -1 with the error set. */
int RunGuarded(Guest *g, GuestFunc *fn, PPCContext &ctx, uint8_t *base, bool thread_entry, uint32_t *exit_code);

/* Call a guest function on the current thread. */
int CallGuest(Guest *g, uint32_t function, const int64_t *args, int nargs, int64_t *result);
/* The same with r12 set (SEH funclets take their establisher frame there). */
int CallGuestR12(Guest *g, uint32_t function, const int64_t *args, int nargs, int64_t *result, uint64_t r12);

/* kernel.cpp: guest exception dispatch */
uint32_t SehResume(PPCContext &ctx, uint8_t *base, uint32_t site);

void StartGuestThread(Guest *g, std::shared_ptr<KThreadObject> obj);

/* Run a host hook for the call the guest is making now (registers in ctx). */
void RunHook(Guest *g, const Hook &h, uint32_t address, PPCContext &ctx);

/* kernel.cpp */
void KernelInit(Guest *g);
void KernelCall(uint32_t library, uint32_t ordinal, PPCContext &ctx, uint8_t *base);
uint32_t KernelVariable(Guest *g, const char *library, uint32_t ordinal, const char *name);
/* Refresh KeTimeStampBundle, which the guest reads directly. */
void UpdateTimeStamp(Guest *g);
uint32_t MakeLdrEntry(Guest *g, uint32_t image_base, uint32_t image_size, uint32_t entry, const char *name,
	const void *xex_header, size_t xex_header_size);
uint32_t CopyXexHeader(Guest *g, const void *data, size_t size);
void ThreadSetup(Guest *g, Thread *t);

} // namespace qaa
