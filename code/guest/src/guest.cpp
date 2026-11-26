/*
 * FnQAA guest runtime: guest memory, threads, calls into the guest,
 * fault handling and module loading.
 */
#include "runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#include <dlfcn.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#endif

#include <xmmintrin.h>

namespace qaa {

thread_local Thread *tls_current_thread = nullptr;

static constexpr uint64_t GUEST_SPACE = 0x100000000ull;
static constexpr uint32_t PAGE = 0x1000;

/* ==== memory ================================================================ */

bool Memory::Reserve() {
	committed.assign(GUEST_SPACE / PAGE, 0);
#ifdef _WIN32
	base = (uint8_t *)VirtualAlloc(nullptr, GUEST_SPACE, MEM_RESERVE, PAGE_NOACCESS);
#else
	void *p = mmap(nullptr, GUEST_SPACE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	base = p == MAP_FAILED ? nullptr : (uint8_t *)p;
#endif
	return base != nullptr;
}

void Memory::Release() {
	if (!base)
		return;
#ifdef _WIN32
	VirtualFree(base, 0, MEM_RELEASE);
#else
	munmap(base, GUEST_SPACE);
#endif
	base = nullptr;
}

bool Memory::Commit(uint32_t address, uint32_t size) {
	if (!size)
		return true;
	uint64_t first = address & ~(uint64_t)(PAGE - 1);
	uint64_t last = ((uint64_t)address + size + PAGE - 1) & ~(uint64_t)(PAGE - 1);
	if (last > GUEST_SPACE)
		return false;
	std::lock_guard<std::mutex> l(lock);
#ifdef _WIN32
	if (!VirtualAlloc(base + first, (size_t)(last - first), MEM_COMMIT, PAGE_READWRITE))
		return false;
#else
	if (mprotect(base + first, (size_t)(last - first), PROT_READ | PROT_WRITE))
		return false;
#endif
	for (uint64_t p = first; p < last; p += PAGE)
		committed[p / PAGE] = 1;
	return true;
}

void Memory::Decommit(uint32_t address, uint32_t size) {
	uint64_t first = address & ~(uint64_t)(PAGE - 1);
	uint64_t last = ((uint64_t)address + size + PAGE - 1) & ~(uint64_t)(PAGE - 1);
	if (last > GUEST_SPACE || last <= first)
		return;
	std::lock_guard<std::mutex> l(lock);
#ifdef _WIN32
	VirtualFree(base + first, (size_t)(last - first), MEM_DECOMMIT);
#else
	madvise(base + first, (size_t)(last - first), MADV_DONTNEED);
	mprotect(base + first, (size_t)(last - first), PROT_NONE);
#endif
	for (uint64_t p = first; p < last; p += PAGE)
		committed[p / PAGE] = 0;
}

bool Memory::IsCommitted(uint32_t address, uint32_t size) const {
	if (!size)
		return true;
	uint64_t last = (uint64_t)address + size;
	if (last > GUEST_SPACE)
		return false;
	for (uint64_t p = address / PAGE; p <= (last - 1) / PAGE; p++)
		if (!committed[p])
			return false;
	return true;
}

std::string Memory::String(uint32_t a, size_t max) const {
	std::string s;
	while (s.size() < max && IsCommitted(a, 1)) {
		char c = (char)base[a++];
		if (!c)
			break;
		s += c;
	}
	return s;
}

/* ==== pool ===================================================================== */

uint32_t Pool::Alloc(uint32_t size, uint32_t align) {
	std::lock_guard<std::mutex> l(lock);
	size = (std::max<uint32_t>(size, 1) + 15) & ~15u;
	if (align < 16)
		align = 16;
	for (auto it = free_blocks.begin(); it != free_blocks.end(); ++it) {
		uint32_t a = (it->first + align - 1) & ~(align - 1);
		if (a + (uint64_t)size > (uint64_t)it->first + it->second)
			continue;
		uint32_t blk = it->first, blk_size = it->second;
		free_blocks.erase(it);
		if (a > blk)
			free_blocks[blk] = a - blk;
		if (a + size < blk + blk_size)
			free_blocks[a + size] = blk + blk_size - (a + size);
		used[a] = size;
		memset(memory->Ptr(a), 0, size);
		return a;
	}
	uint32_t a = (top + align - 1) & ~(align - 1);
	if ((uint64_t)a + size > end)
		return 0;
	if (a > top)
		free_blocks[top] = a - top;
	uint32_t new_top = a + size;
	while (committed_top < new_top) {	/* commit in 64 KiB steps */
		if (!memory->Commit(committed_top, 0x10000))
			return 0;
		committed_top += 0x10000;
	}
	top = new_top;
	used[a] = size;
	memset(memory->Ptr(a), 0, size);
	return a;
}

void Pool::Free(uint32_t address) {
	std::lock_guard<std::mutex> l(lock);
	auto it = used.find(address);
	if (it == used.end())
		return;
	uint32_t size = it->second;
	used.erase(it);
	auto next = free_blocks.lower_bound(address);
	if (next != free_blocks.end() && next->first == address + size) {
		size += next->second;
		next = free_blocks.erase(next);
	}
	if (next != free_blocks.begin()) {
		auto prev = std::prev(next);
		if (prev->first + prev->second == address) {
			prev->second += size;
			return;
		}
	}
	free_blocks[address] = size;
}

/* ==== virtual memory arena ============================================================ */

enum : uint32_t {
	MEM_COMMIT_ = 0x1000, MEM_RESERVE_ = 0x2000, MEM_DECOMMIT_ = 0x4000, MEM_RELEASE_ = 0x8000,
	STATUS_SUCCESS_ = 0, STATUS_NO_MEMORY_ = 0xC0000017, STATUS_INVALID_PARAMETER_ = 0xC000000D,
	STATUS_CONFLICTING_ADDRESSES_ = 0xC0000018, STATUS_MEMORY_NOT_ALLOCATED_ = 0xC00000A0,
};

void VirtualArena::Init(Memory *m, uint32_t b, uint32_t e) {
	memory = m;
	begin = b;
	end = e;
	pages.assign((e - b) / PAGE, FREE);
}

uint32_t VirtualArena::Allocate(uint32_t *address, uint32_t *size, uint32_t type, bool top_down) {
	std::lock_guard<std::mutex> l(lock);
	uint32_t want = *size;
	if (!want)
		return STATUS_INVALID_PARAMETER_;
	uint32_t a = *address;
	if (!a) {
		if (!(type & (MEM_RESERVE_ | MEM_COMMIT_)))
			return STATUS_INVALID_PARAMETER_;
		/* new reservation, 64 KiB aligned and sized */
		uint32_t n = (want + 0xFFFF) & ~0xFFFFu;
		uint32_t pn = n / PAGE;
		uint32_t found = 0;
		const uint32_t gran = 0x10000 / PAGE;
		auto fits = [&](uint32_t p) {
			for (uint32_t i = 0; i < pn; i++)
				if (pages[p + i] != FREE)
					return false;
			return true;
		};
		if (top_down) {
			for (int64_t p = (int64_t)(pages.size() - pn) / gran * gran; p >= 0; p -= gran)
				if (fits((uint32_t)p)) { found = begin + (uint32_t)p * PAGE; break; }
		} else {
			for (uint32_t p = 0; p + pn <= pages.size(); p += gran)
				if (fits(p)) { found = begin + p * PAGE; break; }
		}
		if (!found)
			return STATUS_NO_MEMORY_;
		for (uint32_t i = 0; i < pn; i++)
			pages[(found - begin) / PAGE + i] = RESERVED;
		regions[found] = n;
		a = found;
		want = n;
		if (type & MEM_COMMIT_) {
			if (!memory->Commit(a, want))
				return STATUS_NO_MEMORY_;
			for (uint32_t i = 0; i < pn; i++)
				pages[(a - begin) / PAGE + i] = COMMITTED;
		}
		*address = a;
		*size = want;
		return STATUS_SUCCESS_;
	}
	/* commit (or re-reserve) inside an existing reservation. MEM_LARGE_PAGES
	 * asks for the Xbox's 64 KiB pages: the range grows to whole 64 KiB
	 * pages, which the title's NT heap (default.xex's CRT) relies on. */
	if (a < begin || (uint64_t)a + want > end)
		return STATUS_CONFLICTING_ADDRESSES_;
	const uint32_t page = (type & 0x20000000u) ? 0x10000u : PAGE;	/* MEM_LARGE_PAGES */
	uint32_t first = a & ~(page - 1);
	uint32_t last = (uint32_t)(((uint64_t)a + want + page - 1) & ~(uint64_t)(page - 1));
	if (last > end)
		last = end;
	for (uint32_t p = first; p < last; p += PAGE)
		if (pages[(p - begin) / PAGE] == FREE)
			return STATUS_CONFLICTING_ADDRESSES_;
	if (type & MEM_COMMIT_) {
		if (!memory->Commit(first, last - first))
			return STATUS_NO_MEMORY_;
		for (uint32_t p = first; p < last; p += PAGE)
			pages[(p - begin) / PAGE] = COMMITTED;
	}
	*address = first;
	*size = last - first;
	return STATUS_SUCCESS_;
}

uint32_t VirtualArena::Free(uint32_t *address, uint32_t *size, uint32_t type) {
	std::lock_guard<std::mutex> l(lock);
	uint32_t a = *address;
	if (!Owns(a))
		return STATUS_MEMORY_NOT_ALLOCATED_;
	if (type & MEM_RELEASE_) {
		auto it = regions.upper_bound(a);
		if (it == regions.begin())
			return STATUS_MEMORY_NOT_ALLOCATED_;
		--it;
		if (a >= it->first + it->second)
			return STATUS_MEMORY_NOT_ALLOCATED_;
		uint32_t rb = it->first, rs = it->second;
		memory->Decommit(rb, rs);
		for (uint32_t i = 0; i < rs / PAGE; i++)
			pages[(rb - begin) / PAGE + i] = FREE;
		regions.erase(it);
		*address = rb;
		*size = rs;
		return STATUS_SUCCESS_;
	}
	if (type & MEM_DECOMMIT_) {
		/* The pages stay committed. The title's NT heap (default.xex's CRT,
		 * 0x821D47E8) decommits free space on 64 KiB boundaries and then
		 * writes block headers that its arithmetic places inside the range
		 * it just gave back; on the Xbox that worked because its heap runs
		 * on 64 KiB pages. Keeping the memory costs only address space. */
		uint32_t first = a & ~(PAGE - 1);
		uint32_t last = (uint32_t)(((uint64_t)a + *size + PAGE - 1) & ~(uint64_t)(PAGE - 1));
		if (last > end)
			return STATUS_INVALID_PARAMETER_;
		*address = first;
		*size = last - first;
		return STATUS_SUCCESS_;
	}
	return STATUS_INVALID_PARAMETER_;
}

bool VirtualArena::Query(uint32_t a, uint32_t out[7]) {
	/* MEMORY_BASIC_INFORMATION: BaseAddress, AllocationBase,
	 * AllocationProtect, RegionSize, State, Protect, Type */
	std::lock_guard<std::mutex> l(lock);
	if (!Owns(a))
		return false;
	uint32_t page = (a - begin) / PAGE;
	uint8_t st = pages[page];
	uint32_t p = page;
	while (p < pages.size() && pages[p] == st)
		p++;
	uint32_t alloc_base = 0;
	auto it = regions.upper_bound(a);
	if (it != regions.begin()) {
		--it;
		if (a < it->first + it->second)
			alloc_base = it->first;
	}
	out[0] = begin + page * PAGE;
	out[1] = alloc_base;
	out[2] = st == FREE ? 0 : 0x04;			/* PAGE_READWRITE */
	out[3] = (p - page) * PAGE;
	out[4] = st == FREE ? 0x10000 : st == RESERVED ? 0x2000 : 0x1000;	/* MEM_FREE / RESERVE / COMMIT */
	out[5] = st == COMMITTED ? 0x04 : 0;
	out[6] = st == FREE ? 0 : 0x20000;		/* MEM_PRIVATE */
	return true;
}

/* ==== guest state ===================================================================== */

void Guest::Logv(int level, const char *fmt, va_list ap) {
	char buf[2048];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	if (host.log)
		host.log(host.user, level, buf);
	else
		fprintf(stderr, "[qaa_guest] %s\n", buf);
}

void Guest::Log(int level, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	Logv(level, fmt, ap);
	va_end(ap);
}

void Guest::SetError(const std::string &e) {
	std::lock_guard<std::mutex> l(error_lock);
	last_error = e;
}

void Guest::Register(uint32_t address, GuestFunc *fn) {
	GuestFunc **&page = pages[address >> 16];
	if (!page)
		page = new GuestFunc *[FUNC_PAGE_ENTRIES]();
	page[(address & 0xFFFF) >> 2] = fn;
}

uint32_t Guest::AddHandle(std::shared_ptr<KObject> obj) {
	std::lock_guard<std::mutex> l(handle_lock);
	uint32_t h = next_handle;
	next_handle += 4;
	handles[h] = std::move(obj);
	return h;
}

std::shared_ptr<KObject> Guest::GetHandle(uint32_t handle) {
	std::lock_guard<std::mutex> l(handle_lock);
	auto it = handles.find(handle);
	return it == handles.end() ? nullptr : it->second;
}

bool Guest::CloseHandle(uint32_t handle) {
	std::shared_ptr<KObject> obj;
	{
		std::lock_guard<std::mutex> l(handle_lock);
		auto it = handles.find(handle);
		if (it == handles.end())
			return false;
		obj = std::move(it->second);
		handles.erase(it);
	}
	return true;	/* obj released outside the lock */
}

uint32_t Guest::AllocStack(uint32_t size) {
	std::lock_guard<std::mutex> l(stack_lock);
	size = (size + 0xFFFF) & ~0xFFFFu;
	uint32_t guard = 0x10000;
	if ((uint64_t)next_stack + guard + size > QAA_GUEST_STACK_END)
		return 0;
	uint32_t a = next_stack + guard;
	if (!memory.Commit(a, size))
		return 0;
	next_stack = a + size;
	return a;
}

Thread *Guest::NewThread(uint32_t stack_size) {
	Thread *t = new Thread;
	t->guest = this;
	t->ctx = new PPCContext{};
	t->stack_size = std::max<uint32_t>(stack_size, 0x10000);
	t->stack_base = AllocStack(t->stack_size);
	if (!t->stack_base) {
		delete t;
		return nullptr;
	}
	t->thread_id = next_thread_id++;
	ThreadSetup(this, t);
	return t;
}

Thread::~Thread() { delete ctx; }


static thread_local std::vector<std::pair<Guest *, Thread *>> tls_host_threads;

Thread *Guest::CurrentThread() {
	if (tls_current_thread && tls_current_thread->guest == this)
		return tls_current_thread;
	for (auto &p : tls_host_threads)
		if (p.first == this)
			return tls_current_thread = p.second;
	Thread *t = NewThread(0x100000);
	if (t)
		tls_host_threads.push_back({ this, t });
	return tls_current_thread = t;
}

/* ==== faults ============================================================================ */

static thread_local char tls_fault[1024];
static thread_local uint32_t tls_exit_code;
static thread_local uintptr_t tls_fault_pc;	/* host pc of a hardware fault, else 0 */

#ifdef _WIN32
enum : DWORD { EXC_FATAL = 0xE0514101, EXC_THREAD_EXIT = 0xE0514102 };

[[noreturn]] void Fatal(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tls_fault, sizeof(tls_fault), fmt, ap);
	va_end(ap);
	RaiseException(EXC_FATAL, EXCEPTION_NONCONTINUABLE, 0, nullptr);
	abort();
}

[[noreturn]] void ExitThread(uint32_t code) {
	tls_exit_code = code;
	RaiseException(EXC_THREAD_EXIT, EXCEPTION_NONCONTINUABLE, 0, nullptr);
	abort();
}

static LONG FaultFilter(EXCEPTION_POINTERS *ep, uint8_t *base, bool thread_entry, int *kind) {
	const EXCEPTION_RECORD *er = ep->ExceptionRecord;
	*kind = -1;
	tls_fault_pc = (er->ExceptionCode & 0xFFFFFF00) == 0xE0514100 ? 0 : (uintptr_t)er->ExceptionAddress;
	switch (er->ExceptionCode) {
	case EXC_FATAL:
		return EXCEPTION_EXECUTE_HANDLER;
	case EXC_THREAD_EXIT:
		if (thread_entry) {
			*kind = 1;
		} else {
			snprintf(tls_fault, sizeof(tls_fault), "ExTerminateThread called on a host thread");
		}
		return EXCEPTION_EXECUTE_HANDLER;
	case EXCEPTION_ACCESS_VIOLATION: {
		uintptr_t a = (uintptr_t)er->ExceptionInformation[1];
		const char *op = er->ExceptionInformation[0] == 1 ? "writing" : er->ExceptionInformation[0] == 8 ? "executing" : "reading";
		if (a >= (uintptr_t)base && a < (uintptr_t)base + GUEST_SPACE)
			snprintf(tls_fault, sizeof(tls_fault), "guest memory fault %s unmapped guest address %08X (host pc %p)",
				op, (uint32_t)(a - (uintptr_t)base), er->ExceptionAddress);
		else
			snprintf(tls_fault, sizeof(tls_fault), "host access violation %s %p (host pc %p)", op, (void *)a, er->ExceptionAddress);
		return EXCEPTION_EXECUTE_HANDLER;
	}
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
		snprintf(tls_fault, sizeof(tls_fault), "integer division by zero (host pc %p)", er->ExceptionAddress);
		return EXCEPTION_EXECUTE_HANDLER;
	case EXCEPTION_INT_OVERFLOW:
		snprintf(tls_fault, sizeof(tls_fault), "integer division overflow (host pc %p)", er->ExceptionAddress);
		return EXCEPTION_EXECUTE_HANDLER;
	case EXCEPTION_STACK_OVERFLOW:
		snprintf(tls_fault, sizeof(tls_fault), "host stack overflow in guest code (host pc %p)", er->ExceptionAddress);
		return EXCEPTION_EXECUTE_HANDLER;
	case EXCEPTION_ILLEGAL_INSTRUCTION:
		snprintf(tls_fault, sizeof(tls_fault), "illegal instruction (host pc %p)", er->ExceptionAddress);
		return EXCEPTION_EXECUTE_HANDLER;
	default:
		return EXCEPTION_CONTINUE_SEARCH;
	}
}

/* No C++ objects with destructors here: __try needs a plain frame. */
static int RunGuardedRaw(GuestFunc *fn, PPCContext &ctx, uint8_t *base, bool thread_entry) {
	int kind = -1;
	__try {
		fn(ctx, base);
		return 0;
	} __except (FaultFilter(GetExceptionInformation(), base, thread_entry, &kind)) {
		return kind;
	}
}
#else
/* POSIX: faults raised by the runtime unwind with siglongjmp; hardware
 * faults in guest code are caught through SIGSEGV/SIGBUS/SIGFPE. */
struct JumpFrame {
	sigjmp_buf buf;
	JumpFrame *prev;
	uint8_t *base;
	bool thread_entry;
};
static thread_local JumpFrame *tls_frame;

[[noreturn]] void Fatal(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tls_fault, sizeof(tls_fault), fmt, ap);
	va_end(ap);
	if (tls_frame)
		siglongjmp(tls_frame->buf, 2);
	fprintf(stderr, "qaa_guest fatal: %s\n", tls_fault);
	abort();
}

[[noreturn]] void ExitThread(uint32_t code) {
	tls_exit_code = code;
	if (tls_frame)
		siglongjmp(tls_frame->buf, 3);
	abort();
}

static void SignalHandler(int sig, siginfo_t *si, void *) {
	if (!tls_frame) {
		signal(sig, SIG_DFL);
		raise(sig);
		return;
	}
	uintptr_t a = (uintptr_t)si->si_addr, b = (uintptr_t)tls_frame->base;
	if (sig == SIGFPE)
		snprintf(tls_fault, sizeof(tls_fault), "arithmetic fault in guest code");
	else if (a >= b && a < b + GUEST_SPACE)
		snprintf(tls_fault, sizeof(tls_fault), "guest memory fault at unmapped guest address %08X", (uint32_t)(a - b));
	else
		snprintf(tls_fault, sizeof(tls_fault), "host memory fault at %p", (void *)a);
	siglongjmp(tls_frame->buf, 2);
}

static int RunGuardedRaw(GuestFunc *fn, PPCContext &ctx, uint8_t *base, bool thread_entry) {
	static std::once_flag once;
	std::call_once(once, [] {
		struct sigaction sa = {};
		sa.sa_sigaction = SignalHandler;
		sa.sa_flags = SA_SIGINFO | SA_NODEFER;
		sigaction(SIGSEGV, &sa, nullptr);
		sigaction(SIGBUS, &sa, nullptr);
		sigaction(SIGFPE, &sa, nullptr);
	});
	JumpFrame f;
	f.prev = tls_frame;
	f.base = base;
	f.thread_entry = thread_entry;
	tls_frame = &f;
	int r = sigsetjmp(f.buf, 1);
	if (r == 0) {
		fn(ctx, base);
		tls_frame = f.prev;
		return 0;
	}
	tls_frame = f.prev;
	if (r == 3) {
		if (thread_entry)
			return 1;
		snprintf(tls_fault, sizeof(tls_fault), "ExTerminateThread called on a host thread");
	}
	return -1;
}
#endif

/* The recompiled function whose host code most likely holds pc: the one
 * with the highest entry address not above it (functions are emitted in
 * address order, so this is right unless pc is in runtime code). */
static uint32_t NearestGuestFunction(const Guest *g, uintptr_t pc, uintptr_t *offset) {
	uintptr_t best = 0;
	uint32_t guest = 0;
	for (uint32_t p = 0; p < FUNC_PAGE_COUNT; p++) {
		GuestFunc *const *page = g->pages[p];
		if (!page)
			continue;
		for (uint32_t i = 0; i < FUNC_PAGE_ENTRIES; i++) {
			uintptr_t h = (uintptr_t)page[i];
			if (h && h <= pc && h > best) {
				best = h;
				guest = (p << 16) | (i << 2);
			}
		}
	}
	*offset = pc - best;
	return guest;
}

int RunGuarded(Guest *g, GuestFunc *fn, PPCContext &ctx, uint8_t *base, bool thread_entry, uint32_t *exit_code) {
	tls_fault[0] = 0;
	tls_fault_pc = 0;
	int r = RunGuardedRaw(fn, ctx, base, thread_entry);
	if (r == 1 && exit_code)
		*exit_code = tls_exit_code;
	if (r < 0) {
		char msg[1200];
		char where[96] = "";
		if (tls_fault_pc) {
			uintptr_t off = 0;
			uint32_t gf = NearestGuestFunction(g, tls_fault_pc, &off);
			if (gf && off < 0x100000)
				snprintf(where, sizeof(where), ", in or after sub_%08X (+0x%zX host)", gf, (size_t)off);
		}
		snprintf(msg, sizeof(msg), "%s (guest lr %08X, r1 %08X%s)", tls_fault[0] ? tls_fault : "unknown fault",
			(uint32_t)ctx.lr, ctx.r1.u32, where);
		g->SetError(msg);
	}
	return r;
}

/* ==== calls into the guest ================================================================== */

static int CallGuestEx(Guest *g, Thread *t, uint32_t function, const int64_t *args, int nargs, int64_t *result,
	bool thread_entry, uint32_t *exit_code, const uint64_t *r12 = nullptr,
	const double *fargs = nullptr, int nfargs = 0, double *f1 = nullptr) {
	GuestFunc *fn = g->Lookup(function);
	if (!fn) {
		char e[96];
		snprintf(e, sizeof(e), "no recompiled function at %08X", function);
		g->SetError(e);
		return -1;
	}
	if (nargs < 0 || nargs > 16) {
		g->SetError("too many arguments");
		return -1;
	}
	Thread *prev_thread = tls_current_thread;
	tls_current_thread = t;
	PPCContext &ctx = *t->ctx;
	std::unique_ptr<PPCContext> saved;
	uint32_t sp;
	if (t->depth > 0) {
		/* a nested call (guest -> host -> guest): keep the outer state */
		saved.reset(new PPCContext(ctx));
		sp = (ctx.r1.u32 - 0x400) & ~0xFu;	/* below the caller's red zone */
	} else {
		sp = t->stack_base + t->stack_size;
	}
	int nstack = std::max(0, nargs - 8);
	sp -= (0x50 + 8 * nstack + 15) & ~15;
	g->memory.W32(sp, 0);	/* back chain */
	for (int i = 0; i < 8; i++) {
		int64_t v = i < nargs ? args[i] : 0;
		switch (i) {
		case 0: ctx.r3.s64 = v; break;
		case 1: ctx.r4.s64 = v; break;
		case 2: ctx.r5.s64 = v; break;
		case 3: ctx.r6.s64 = v; break;
		case 4: ctx.r7.s64 = v; break;
		case 5: ctx.r8.s64 = v; break;
		case 6: ctx.r9.s64 = v; break;
		case 7: ctx.r10.s64 = v; break;
		}
	}
	for (int k = 0; k < nstack; k++)
		g->memory.W64(sp + 0x50 + 8 * k, (uint64_t)args[8 + k]);
	{
		PPCRegister *fr = &ctx.f1;
		for (int k = 0; k < nfargs && k < 13; k++)
			fr[k].f64 = fargs[k];
	}
	ctx.r1.u64 = sp;
	ctx.r13.u64 = t->pcr;
	ctx.lr = 0;
	if (r12)
		ctx.r12.u64 = *r12;

	UpdateTimeStamp(g);
	unsigned host_csr = _mm_getcsr();
	if (t->depth == 0)
		ctx.fpscr.loadFromHost();
	t->depth++;
	int r = RunGuarded(g, fn, ctx, g->memory.base, thread_entry, exit_code);
	t->depth--;
	if (r == 0 && result)
		*result = ctx.r3.s64;
	if (r == 0 && f1)
		*f1 = ctx.f1.f64;
	if (saved)
		ctx = *saved;
	_mm_setcsr(host_csr);
	tls_current_thread = prev_thread;
	return r;
}

int CallGuest(Guest *g, uint32_t function, const int64_t *args, int nargs, int64_t *result) {
	Thread *t = g->CurrentThread();
	if (!t) {
		g->SetError("cannot create guest thread state");
		return -1;
	}
	return CallGuestEx(g, t, function, args, nargs, result, false, nullptr) == 0 ? 0 : -1;
}

int CallGuestR12(Guest *g, uint32_t function, const int64_t *args, int nargs, int64_t *result, uint64_t r12) {
	Thread *t = g->CurrentThread();
	if (!t) {
		g->SetError("cannot create guest thread state");
		return -1;
	}
	return CallGuestEx(g, t, function, args, nargs, result, false, nullptr, &r12) == 0 ? 0 : -1;
}

/* Entry of a thread the guest created (ExCreateThread). */
static void GuestThreadMain(Guest *g, std::shared_ptr<KThreadObject> obj) {
	Thread *t = obj->thread;
	tls_current_thread = t;
	{
		std::unique_lock<std::mutex> l(g->wait_lock);
		g->wait_cv.wait(l, [&] { return obj->suspend_count == 0; });
	}
	int64_t args[2];
	int n;
	uint32_t fn;
	if (obj->xapi_startup) {
		fn = obj->xapi_startup;
		args[0] = obj->start_address;
		args[1] = obj->start_context;
		n = 2;
	} else {
		fn = obj->start_address;
		args[0] = obj->start_context;
		n = 1;
	}
	int64_t result = 0;
	uint32_t code = 0;
	int r = CallGuestEx(g, t, fn, args, n, &result, true, &code);
	if (r == 0)
		code = (uint32_t)result;
	else if (r < 0)
		g->Log(QAA_LOG_ERROR, "guest thread %u (start %08X) failed: %s", t->thread_id, obj->start_address,
			qaa_guest_last_error((qaa_guest *)g));
	{
		std::lock_guard<std::mutex> l(g->wait_lock);
		obj->exit_code = code;
		obj->signaled = true;
	}
	g->wait_cv.notify_all();
	tls_current_thread = nullptr;
}

/* Recompiled code takes one host frame per guest frame, often larger than
 * the guest's, so guest threads get a generous host stack (reserved, not
 * committed). Host threads that call into the guest need the same. */
static constexpr size_t GUEST_THREAD_HOST_STACK = 16u << 20;

struct ThreadStart {
	Guest *g;
	std::shared_ptr<KThreadObject> obj;
};

#ifdef _WIN32
static unsigned __stdcall ThreadTrampoline(void *p) {
	std::unique_ptr<ThreadStart> s((ThreadStart *)p);
	GuestThreadMain(s->g, s->obj);
	return 0;
}
#else
static void *ThreadTrampoline(void *p) {
	std::unique_ptr<ThreadStart> s((ThreadStart *)p);
	GuestThreadMain(s->g, s->obj);
	return nullptr;
}
#endif

void StartGuestThread(Guest *g, std::shared_ptr<KThreadObject> obj) {
	auto *s = new ThreadStart{ g, obj };
	bool ok;
#ifdef _WIN32
	HANDLE h = (HANDLE)_beginthreadex(nullptr, (unsigned)GUEST_THREAD_HOST_STACK, ThreadTrampoline, s,
		STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
	ok = h != nullptr;
	if (h)
		CloseHandle(h);
#else
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, GUEST_THREAD_HOST_STACK);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_t t;
	ok = pthread_create(&t, &attr, ThreadTrampoline, s) == 0;
	pthread_attr_destroy(&attr);
#endif
	if (!ok) {
		delete s;
		g->Log(QAA_LOG_ERROR, "cannot start a host thread for guest thread %u", obj->thread->thread_id);
		{
			std::lock_guard<std::mutex> l(g->wait_lock);
			obj->exit_code = 0xC0000017;	/* STATUS_NO_MEMORY */
			obj->signaled = true;
		}
		g->wait_cv.notify_all();
	}
}

/* ==== host calls and module callbacks ========================================================= */

static void IndirectMiss(PPCContext &ctx, uint8_t *base, uint32_t target) {
	Thread *t = tls_current_thread;
	Guest *g = t ? t->guest : nullptr;
	if (g && target >= QAA_GUEST_HOSTCALL_BASE && target < QAA_GUEST_HOSTCALL_END) {
		uint32_t index = (target - QAA_GUEST_HOSTCALL_BASE) / 4;
		if (index < g->host_calls.size() && g->host_calls[index].fn) {
			const HostCall hc = g->host_calls[index];
			qaa_syscall call;
			call.trap = ctx.r3.s32;
			call.args[0] = ctx.r4.s32;
			call.args[1] = ctx.r5.s32;
			call.args[2] = ctx.r6.s32;
			call.args[3] = ctx.r7.s32;
			call.args[4] = ctx.r8.s32;
			call.args[5] = ctx.r9.s32;
			call.args[6] = ctx.r10.s32;
			for (int k = 0; k < QAA_SYSCALL_MAX_ARGS - 7; k++) {
				uint32_t a = ctx.r1.u32 + 0x54 + 8 * k;	/* low word of the k-th stack slot */
				call.args[7 + k] = g->memory.IsCommitted(a, 4) ? (int32_t)g->memory.U32(a) : 0;
			}
			const PPCRegister *f = &ctx.f1;
			for (int k = 0; k < 13; k++)
				call.fargs[k] = f[k].f64;
			call.return_address = (uint32_t)ctx.lr;
			ctx.r3.s64 = hc.fn(hc.user, hc.module, &call);
			return;
		}
	}
	if (g) {
		Hook h;
		{
			std::lock_guard<std::mutex> l(g->hook_lock);
			auto it = g->hooks.find(target);
			h = it != g->hooks.end() ? it->second : g->default_hook;
		}
		if (h.fn) {
			RunHook(g, h, target, ctx);
			return;
		}
	}
	Fatal("indirect call to %08X, which is no recompiled function (lr %08X)", target, (uint32_t)ctx.lr);
}

void RunHook(Guest *g, const Hook &h, uint32_t address, PPCContext &ctx) {
	UpdateTimeStamp(g);
	qaa_hook_call call;
	call.address = address;
	call.return_address = (uint32_t)ctx.lr;
	call.sp = ctx.r1.u32;
	/* PPCContext keeps r3 first and r0..r2 after it: name each register */
	PPCRegister *const regs[8] = { &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10 };
	for (int k = 0; k < 8; k++)
		call.r[k] = regs[k]->u64;
	const PPCRegister *f = &ctx.f1;
	for (int k = 0; k < 13; k++)
		call.f[k] = f[k].f64;
	h.fn(h.user, &call);
	ctx.r3.u64 = call.r[0];
	ctx.f1.f64 = call.f[0];
}

static void TrapHook(PPCContext &ctx, uint8_t *, uint32_t address) {
	Fatal("guest trap instruction at %08X (divide by zero check or debug break)", address);
}

} // namespace qaa

using namespace qaa;

/* ==== C API ===================================================================================== */

struct qaa_guest : Guest {};

extern "C" {

qaa_guest *qaa_guest_create(const qaa_host *host, char *error, size_t error_size) {
	auto *g = new qaa_guest;
	if (host)
		g->host = *host;
	if (!g->memory.Reserve()) {
		if (error)
			snprintf(error, error_size, "cannot reserve the 4 GiB guest address space");
		delete g;
		return nullptr;
	}
	g->pool.Init(&g->memory, QAA_GUEST_KERNEL_BASE, QAA_GUEST_KERNEL_END);
	g->arena.Init(&g->memory, QAA_GUEST_HEAP_BASE, QAA_GUEST_HEAP_END);
	g->module_host.abi_version = QAA_MODULE_ABI_VERSION;
	g->module_host.kernel_call = KernelCall;
	g->module_host.func_pages = g->pages;
	g->module_host.indirect_miss = IndirectMiss;
	g->module_host.trap = TrapHook;
	g->module_host.seh_resume = SehResume;
	g->host_calls.reserve((QAA_GUEST_HOSTCALL_END - QAA_GUEST_HOSTCALL_BASE) / 4);
	KernelInit(g);
	return g;
}

void qaa_guest_destroy(qaa_guest *g) {
	if (!g)
		return;
	/* Guest threads are detached host threads. If one is still running (an
	 * idle worker parked in a wait, like cgame's), freeing the guest would
	 * pull its memory and wait state out from under it: leave the guest
	 * allocated instead. The process exit reclaims it. */
	{
		std::lock_guard<std::mutex> l(g->wait_lock);
		size_t running = 0;
		for (auto &t : g->threads)
			running += !t->signaled;
		if (running) {
			g->Log(QAA_LOG_WARNING, "qaa_guest_destroy: %zu guest thread(s) still running; the guest stays allocated", running);
			return;
		}
	}
	for (auto &m : g->modules) {
#ifdef _WIN32
		if (m->library)
			FreeLibrary((HMODULE)m->library);
#else
		if (m->library)
			dlclose(m->library);
#endif
	}
	for (auto &p : g->pages)
		delete[] p;
	g->memory.Release();
	delete g;
}

const char *qaa_guest_last_error(const qaa_guest *g) {
	return g ? g->last_error.c_str() : "no guest";
}

static void *OpenLibrary(const char *path, std::string &err) {
#ifdef _WIN32
	HMODULE h = LoadLibraryA(path);
	if (!h)
		err = "LoadLibrary failed with error " + std::to_string(GetLastError());
	return h;
#else
	void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!h)
		err = dlerror();
	return h;
#endif
}

static void *LibrarySymbol(void *lib, const char *name) {
#ifdef _WIN32
	return (void *)GetProcAddress((HMODULE)lib, name);
#else
	return dlsym(lib, name);
#endif
}

qaa_module *qaa_module_load(qaa_guest *g, const char *path, char *error, size_t error_size) {
	std::string err;
	auto fail = [&](const std::string &e) -> qaa_module * {
		if (error)
			snprintf(error, error_size, "%s: %s", path, e.c_str());
		g->SetError(e);
		return nullptr;
	};
	void *lib = OpenLibrary(path, err);
	if (!lib)
		return fail(err);
	auto describe = (qaa_module_describe_fn)LibrarySymbol(lib, QAA_MODULE_DESCRIBE_SYMBOL);
	if (!describe)
		return fail("not a recompiled module (no " QAA_MODULE_DESCRIBE_SYMBOL ")");
	const ModuleDesc *d = describe();
	if (d->abi_version != QAA_MODULE_ABI_VERSION)
		return fail("module ABI version " + std::to_string(d->abi_version) + ", runtime " + std::to_string(QAA_MODULE_ABI_VERSION));
	if (d->context_size != sizeof(PPCContext) || d->config_flags != PpcConfigFlags())
		return fail("module was recompiled with a different PPCContext layout");
	if (d->image_base < QAA_GUEST_IMAGE_BASE || (uint64_t)d->image_base + d->image_size > QAA_GUEST_IMAGE_END)
		return fail("image outside the image region");
	for (auto &m : g->modules)
		if (d->image_base < m->desc->image_base + m->desc->image_size && m->desc->image_base < d->image_base + d->image_size)
			return fail("image overlaps " + m->name);

	/* map the image: stored data, then zero up to the image size (BSS) */
	if (!g->memory.Commit(d->image_base, d->image_size))
		return fail("cannot commit the image");
	memcpy(g->memory.Ptr(d->image_base), d->image_data, std::min(d->image_data_size, d->image_size));
	memset(g->memory.Ptr(d->image_base + d->image_data_size), 0,
		d->image_size > d->image_data_size ? d->image_size - d->image_data_size : 0);

	for (const FuncMapping *f = d->funcs; f->host; f++)
		g->Register((uint32_t)f->guest, f->host);

	/* imports: variables get the kernel object's address, functions the
	 * thunk (which the recompiled code reaches as __imp__<name>) */
	for (uint32_t i = 0; i < d->import_count; i++) {
		const ModuleImport &imp = d->imports[i];
		if (!imp.slot)
			continue;
		uint32_t value = 0;
		if (imp.kind == IMPORT_VARIABLE) {
			value = KernelVariable(g, imp.library, imp.ordinal, imp.name);
			if (!value)
				g->Log(QAA_LOG_WARNING, "%s: no kernel variable for import %s (%s ordinal %u)", d->name, imp.name, imp.library, imp.ordinal);
		} else {
			value = imp.thunk;
		}
		g->memory.W32(imp.slot, value);
	}
	d->bind(&g->module_host);

	auto m = std::make_unique<qaa_module>();
	m->guest = g;
	m->library = lib;
	m->desc = d;
	m->name = d->name;
	m->ldr_entry = MakeLdrEntry(g, d->image_base, d->image_size, d->entry_point, d->original_name,
		d->xex_header, d->xex_header_size);
	qaa_module *mp = m.get();
	g->modules.push_back(std::move(m));
	g->Log(QAA_LOG_INFO, "%s: mapped %08X-%08X (%u bytes stored), %s", d->name, d->image_base,
		d->image_base + d->image_size, d->image_data_size, d->recompiler);

	/* CRT start-up: _DllMainCRTStartup(hModule, DLL_PROCESS_ATTACH, NULL) */
	if (d->entry_point) {
		int64_t args[3] = { (int64_t)mp->ldr_entry, 1, 0 };
		int64_t ok = 0;
		if (CallGuest(g, d->entry_point, args, 3, &ok))
			return fail(std::string("entry point failed: ") + g->last_error);
		if (!(int32_t)ok)
			return fail("entry point returned FALSE");
	}
	return mp;
}

void qaa_module_unload(qaa_module *m) {
	if (!m)
		return;
	Guest *g = m->guest;
	if (m->desc->entry_point) {
		int64_t args[3] = { (int64_t)m->ldr_entry, 0, 0 };	/* DLL_PROCESS_DETACH */
		int64_t r;
		CallGuest(g, m->desc->entry_point, args, 3, &r);
	}
	for (const FuncMapping *f = m->desc->funcs; f->host; f++)
		g->Register((uint32_t)f->guest, nullptr);
	g->memory.Decommit(m->desc->image_base, m->desc->image_size);
	for (auto it = g->modules.begin(); it != g->modules.end(); ++it) {
		if (it->get() == m) {
#ifdef _WIN32
			FreeLibrary((HMODULE)m->library);
#else
			dlclose(m->library);
#endif
			g->modules.erase(it);
			break;
		}
	}
}

const char *qaa_module_name(const qaa_module *m) { return m->name.c_str(); }
uint32_t qaa_module_base(const qaa_module *m) { return m->desc->image_base; }
uint32_t qaa_module_size(const qaa_module *m) { return m->desc->image_size; }
qaa_guest *qaa_module_guest(const qaa_module *m) { return (qaa_guest *)m->guest; }

uint32_t qaa_module_export(const qaa_module *m, uint32_t ordinal) {
	if (!ordinal || ordinal > m->desc->export_count)
		return 0;
	return m->desc->exports[ordinal - 1];
}

uint32_t qaa_module_host_function(qaa_module *m, qaa_syscall_fn fn, void *user) {
	Guest *g = m->guest;
	std::lock_guard<std::mutex> l(g->host_call_lock);
	if (g->host_calls.size() >= g->host_calls.capacity()) {
		g->SetError("out of host-call addresses");
		return 0;
	}
	uint32_t address = QAA_GUEST_HOSTCALL_BASE + 4 * (uint32_t)g->host_calls.size();
	g->host_calls.push_back({ fn, user, m });
	return address;
}

int qaa_module_dll_entry(qaa_module *m, qaa_syscall_fn fn, void *user) {
	Guest *g = m->guest;
	uint32_t entry = qaa_module_export(m, QAA_EXPORT_DLLENTRY);
	if (!entry) {
		g->SetError("module has no dllEntry export");
		return QAA_ERROR;
	}
	if (!m->syscall_address) {
		m->syscall_address = qaa_module_host_function(m, fn, user);
		if (!m->syscall_address)
			return QAA_ERROR;
	} else {
		std::lock_guard<std::mutex> l(g->host_call_lock);
		HostCall &hc = g->host_calls[(m->syscall_address - QAA_GUEST_HOSTCALL_BASE) / 4];
		hc.fn = fn;
		hc.user = user;
	}
	int64_t args[1] = { (int64_t)m->syscall_address };
	return CallGuest(g, entry, args, 1, nullptr) ? QAA_ERROR : QAA_OK;
}

int qaa_module_vm_main(qaa_module *m, int32_t command, const int32_t *args, int nargs, int64_t *result) {
	Guest *g = m->guest;
	uint32_t entry = qaa_module_export(m, QAA_EXPORT_VMMAIN);
	if (!entry) {
		g->SetError("module has no vmMain export");
		return QAA_ERROR;
	}
	int64_t a[16];
	if (nargs > 15)
		nargs = 15;
	a[0] = command;
	for (int i = 0; i < nargs; i++)
		a[1 + i] = args[i];
	/* vmMain(command, arg0 .. arg11): pass the full set the guest expects */
	int n = std::max(1 + nargs, 13);
	for (int i = 1 + nargs; i < n; i++)
		a[i] = 0;
	int64_t r = 0;
	int rc = CallGuest(g, entry, a, n, &r);
	if (result)
		*result = r;
	return rc ? QAA_ERROR : QAA_OK;
}

int qaa_guest_set_executable_header(qaa_guest *g, const void *header, size_t size) {
	if (!header || size < 0x18 || size > 0x100000 || memcmp(header, "XEX2", 4)) {
		g->SetError("not a XEX2 header");
		return QAA_ERROR;
	}
	uint32_t hdr = CopyXexHeader(g, header, size);
	if (!hdr) {
		g->SetError("out of kernel pool memory");
		return QAA_ERROR;
	}
	g->exe_xex_header = hdr;
	g->memory.W32(g->exe_ldr_entry + 0x58, hdr);
	return QAA_OK;
}

void qaa_guest_abort(qaa_guest *, const char *message) {
	Fatal("%s", message ? message : "aborted by the host");
}

int qaa_guest_call(qaa_guest *g, uint32_t function, const int64_t *args, int nargs, int64_t *result) {
	return CallGuest(g, function, args, nargs, result) ? QAA_ERROR : QAA_OK;
}

int qaa_guest_call_regs(qaa_guest *g, uint32_t function, const uint64_t *r, int nr, const double *f, int nf,
	uint64_t *r3, double *f1) {
	Thread *t = g->CurrentThread();
	if (!t) {
		g->SetError("cannot create guest thread state");
		return QAA_ERROR;
	}
	if (nr < 0 || nr > 8 || nf < 0 || nf > 13) {
		g->SetError("too many register arguments");
		return QAA_ERROR;
	}
	int64_t a[8] = {};
	for (int k = 0; k < nr; k++)
		a[k] = (int64_t)r[k];
	int64_t res = 0;
	int rc = CallGuestEx(g, t, function, a, nr, &res, false, nullptr, nullptr, f, nf, f1);
	if (rc == 0 && r3)
		*r3 = (uint64_t)res;
	return rc ? QAA_ERROR : QAA_OK;
}

int qaa_guest_set_hook(qaa_guest *g, uint32_t address, qaa_hook_fn fn, void *user) {
	std::lock_guard<std::mutex> l(g->hook_lock);
	if (fn)
		g->hooks[address] = { fn, user };
	else
		g->hooks.erase(address);
	return QAA_OK;
}

void qaa_guest_set_default_hook(qaa_guest *g, qaa_hook_fn fn, void *user) {
	std::lock_guard<std::mutex> l(g->hook_lock);
	g->default_hook = { fn, user };
}

static uint32_t LibraryIndex(const char *library) {
	return library && !strcmp(library, "xam.xex") ? 1 : 0;
}

int qaa_guest_set_import_hook(qaa_guest *g, const char *library, uint32_t ordinal, qaa_hook_fn fn, void *user) {
	std::lock_guard<std::mutex> l(g->hook_lock);
	uint32_t key = LibraryIndex(library) << 16 | (ordinal & 0xFFFF);
	if (fn)
		g->import_hooks[key] = { fn, user };
	else
		g->import_hooks.erase(key);
	return QAA_OK;
}

void qaa_guest_set_default_import_hook(qaa_guest *g, qaa_hook_fn fn, void *user) {
	std::lock_guard<std::mutex> l(g->hook_lock);
	g->default_import_hook = { fn, user };
}

int qaa_guest_wait_thread_blocked(qaa_guest *g, uint32_t thread_handle, uint32_t event_handle, int timeout_ms) {
	auto th = g->GetHandle(thread_handle);
	auto ev = g->GetHandle(event_handle);
	if (!th || th->type != KObject::K_THREAD || !ev)
		return QAA_ERROR;
	auto *kt = static_cast<KThreadObject *>(th.get());
	std::unique_lock<std::mutex> l(g->wait_lock);
	auto ready = [&] { return kt->signaled || (kt->thread && kt->thread->blocked_on == ev.get()); };
	if (timeout_ms < 0) {
		g->wait_cv.wait(l, ready);
		return QAA_OK;
	}
	return g->wait_cv.wait_for(l, std::chrono::milliseconds(timeout_ms), ready) ? QAA_OK : QAA_ERROR;
}

int qaa_guest_step_thread(qaa_guest *g, uint32_t thread_handle, uint32_t event_handle, int timeout_ms) {
	auto th = g->GetHandle(thread_handle);
	auto ev = g->GetHandle(event_handle);
	if (!th || th->type != KObject::K_THREAD || !ev || ev->type != KObject::K_EVENT)
		return QAA_ERROR;
	auto *kt = static_cast<KThreadObject *>(th.get());
	std::unique_lock<std::mutex> l(g->wait_lock);
	if (kt->signaled || !kt->thread)
		return QAA_ERROR;
	/* the thread must be parked on the event: signal it, then wait for it
	 * to enter a new wait on the same event (or exit) */
	auto parked = [&] { return kt->signaled || kt->thread->blocked_on == ev.get(); };
	if (!g->wait_cv.wait_for(l, std::chrono::milliseconds(timeout_ms < 0 ? 600000 : timeout_ms), parked))
		return QAA_ERROR;
	if (kt->signaled)
		return QAA_ERROR;
	const uint64_t before = kt->thread->waits_begun;
	ev->signaled = true;
	g->wait_cv.notify_all();
	auto back = [&] {
		return kt->signaled || (kt->thread->waits_begun > before && kt->thread->blocked_on == ev.get());
	};
	if (timeout_ms < 0) {
		g->wait_cv.wait(l, back);
		return QAA_OK;
	}
	return g->wait_cv.wait_for(l, std::chrono::milliseconds(timeout_ms), back) ? QAA_OK : QAA_ERROR;
}

int qaa_guest_set_event(qaa_guest *g, uint32_t event_handle) {
	auto ev = g->GetHandle(event_handle);
	if (!ev || ev->type != KObject::K_EVENT)
		return QAA_ERROR;
	{
		std::lock_guard<std::mutex> l(g->wait_lock);
		ev->signaled = true;
	}
	g->wait_cv.notify_all();
	return QAA_OK;
}

uint8_t *qaa_guest_base(const qaa_guest *g) { return g->memory.base; }
void *qaa_guest_translate(const qaa_guest *g, uint32_t a) { return g->memory.base + a; }
int qaa_guest_is_mapped(const qaa_guest *g, uint32_t a, uint32_t size) { return g->memory.IsCommitted(a, size); }

uint8_t qaa_guest_load8(const qaa_guest *g, uint32_t a) { return g->memory.U8(a); }
uint16_t qaa_guest_load16(const qaa_guest *g, uint32_t a) { return g->memory.U16(a); }
uint32_t qaa_guest_load32(const qaa_guest *g, uint32_t a) { return g->memory.U32(a); }
uint64_t qaa_guest_load64(const qaa_guest *g, uint32_t a) { return g->memory.U64(a); }
float qaa_guest_loadf(const qaa_guest *g, uint32_t a) { uint32_t v = g->memory.U32(a); float f; memcpy(&f, &v, 4); return f; }
double qaa_guest_loadd(const qaa_guest *g, uint32_t a) { uint64_t v = g->memory.U64(a); double d; memcpy(&d, &v, 8); return d; }
void qaa_guest_store8(qaa_guest *g, uint32_t a, uint8_t v) { g->memory.W8(a, v); }
void qaa_guest_store16(qaa_guest *g, uint32_t a, uint16_t v) { g->memory.W16(a, v); }
void qaa_guest_store32(qaa_guest *g, uint32_t a, uint32_t v) { g->memory.W32(a, v); }
void qaa_guest_store64(qaa_guest *g, uint32_t a, uint64_t v) { g->memory.W64(a, v); }
void qaa_guest_storef(qaa_guest *g, uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); g->memory.W32(a, v); }
void qaa_guest_stored(qaa_guest *g, uint32_t a, double d) { uint64_t v; memcpy(&v, &d, 8); g->memory.W64(a, v); }
void qaa_guest_read(const qaa_guest *g, uint32_t a, void *dst, size_t n) { memcpy(dst, g->memory.base + a, n); }
void qaa_guest_write(qaa_guest *g, uint32_t a, const void *src, size_t n) { memcpy(g->memory.base + a, src, n); }

size_t qaa_guest_strnlen(const qaa_guest *g, uint32_t a, size_t max) {
	size_t n = 0;
	while (n < max && g->memory.IsCommitted(a + (uint32_t)n, 1) && g->memory.base[a + n])
		n++;
	return n;
}

uint32_t qaa_guest_alloc(qaa_guest *g, uint32_t size) { return g->pool.Alloc(size); }
void qaa_guest_free(qaa_guest *g, uint32_t a) { g->pool.Free(a); }

} // extern "C"
