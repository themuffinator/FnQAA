/*
 * FnQAA guest runtime: xboxkrnl high-level emulation.
 *
 * The functions QAA's game modules import (docs/FORMATS.md lists them).
 * Arguments arrive in r3..r10 (and the caller's stack slots at r1+0x54
 * onwards) as guest values; results go back in r3. Structures are the
 * Xbox 360 kernel's, big-endian in guest memory. Signatures and layouts
 * follow the public Xbox 360 kernel interface as documented by xenia.
 */
#include "runtime.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace qaa {

enum : uint32_t {
	STATUS_SUCCESS = 0x00000000,
	STATUS_ABANDONED_WAIT_0 = 0x00000080,
	STATUS_TIMEOUT = 0x00000102,
	STATUS_NO_MORE_FILES = 0x80000006,
	STATUS_UNSUCCESSFUL = 0xC0000001,
	STATUS_NOT_IMPLEMENTED = 0xC0000002,
	STATUS_INFO_LENGTH_MISMATCH = 0xC0000004,
	STATUS_INVALID_HANDLE = 0xC0000008,
	STATUS_INVALID_PARAMETER = 0xC000000D,
	STATUS_NO_SUCH_FILE = 0xC000000F,
	STATUS_END_OF_FILE = 0xC0000011,
	STATUS_NO_MEMORY = 0xC0000017,
	STATUS_ACCESS_DENIED = 0xC0000022,
	STATUS_BUFFER_TOO_SMALL = 0xC0000023,
	STATUS_OBJECT_TYPE_MISMATCH = 0xC0000024,
	STATUS_OBJECT_NAME_INVALID = 0xC0000033,
	STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034,
	STATUS_OBJECT_NAME_COLLISION = 0xC0000035,
	STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A,
	STATUS_FILE_IS_A_DIRECTORY = 0xC00000BA,
	STATUS_NOT_A_DIRECTORY = 0xC0000103,
};

enum : uint32_t {
	H_CURRENT_PROCESS = 0xFFFFFFFF,
	H_CURRENT_THREAD = 0xFFFFFFFE,
};

/* Xbox 360 kernel ordinals (xboxkrnl.exe) of the modules' imports. */
#define QAA_KERNEL_IMPORTS(X) \
	X(0x00D, ExCreateThread) \
	X(0x019, ExTerminateThread) \
	X(0x052, KeBugCheck) \
	X(0x053, KeBugCheckEx) \
	X(0x066, KeGetCurrentProcessType) \
	X(0x083, KeQueryPerformanceFrequency) \
	X(0x084, KeQuerySystemTime) \
	X(0x097, KeSetAffinityThread) \
	X(0x0CC, NtAllocateVirtualMemory) \
	X(0x0CE, NtClearEvent) \
	X(0x0CF, NtClose) \
	X(0x0D1, NtCreateEvent) \
	X(0x0D2, NtCreateFile) \
	X(0x0DA, NtDuplicateObject) \
	X(0x0DB, NtFlushBuffersFile) \
	X(0x0DC, NtFreeVirtualMemory) \
	X(0x0DF, NtOpenFile) \
	X(0x0E4, NtQueryDirectoryFile) \
	X(0x0E7, NtQueryFullAttributesFile) \
	X(0x0E8, NtQueryInformationFile) \
	X(0x0EE, NtQueryVirtualMemory) \
	X(0x0EF, NtQueryVolumeInformationFile) \
	X(0x0F0, NtReadFile) \
	X(0x0F1, NtReadFileScatter) \
	X(0x0F5, NtResumeThread) \
	X(0x0F6, NtSetEvent) \
	X(0x0F7, NtSetInformationFile) \
	X(0x0FD, NtWaitForSingleObjectEx) \
	X(0x0FE, NtWaitForMultipleObjectsEx) \
	X(0x0FF, NtWriteFile) \
	X(0x105, ObDereferenceObject) \
	X(0x110, ObReferenceObjectByHandle) \
	X(0x11B, RtlCompareMemoryUlong) \
	X(0x125, RtlEnterCriticalSection) \
	X(0x127, RtlFreeAnsiString) \
	X(0x12B, RtlImageXexHeaderField) \
	X(0x12C, RtlInitAnsiString) \
	X(0x12D, RtlInitUnicodeString) \
	X(0x12E, RtlInitializeCriticalSection) \
	X(0x12F, RtlInitializeCriticalSectionAndSpinCount) \
	X(0x130, RtlLeaveCriticalSection) \
	X(0x133, RtlMultiByteToUnicodeN) \
	X(0x135, RtlNtStatusToDosError) \
	X(0x136, RtlRaiseException) \
	X(0x141, RtlTryEnterCriticalSection) \
	X(0x142, RtlUnicodeStringToAnsiString) \
	X(0x152, KeTlsAlloc) \
	X(0x153, KeTlsFree) \
	X(0x154, KeTlsGetValue) \
	X(0x155, KeTlsSetValue) \
	X(0x1A5, __C_specific_handler)

enum : uint32_t {
	ORD_ExEventObjectType = 0x00E,
	ORD_KeTimeStampBundle = 0x0AD,
	ORD_XboxKrnlVersion = 0x158,
	ORD_ExThreadObjectType = 0x01B,
	ORD_KeDebugMonitorData = 0x059,
	ORD_XboxHardwareInfo = 0x156,
	ORD_XexExecutableModuleHandle = 0x193,
};

static const char *KernelName(uint32_t ordinal) {
	switch (ordinal) {
#define NAME_CASE(o, n) case o: return #n;
		QAA_KERNEL_IMPORTS(NAME_CASE)
#undef NAME_CASE
	default: return nullptr;
	}
}

/* ---- helpers ------------------------------------------------------------------ */

static inline Guest *G() { return tls_current_thread->guest; }
static inline Memory &M() { return G()->memory; }

static inline uint32_t StackArg(PPCContext &ctx, int k) {	/* k-th argument after r10 */
	return M().U32(ctx.r1.u32 + 0x54 + 8 * k);
}

static void IoStatus(uint32_t iosb, uint32_t status, uint32_t information) {
	if (iosb) {
		M().W32(iosb, status);
		M().W32(iosb + 4, information);
	}
}

/* X_ANSI_STRING { u16 Length; u16 MaximumLength; u32 Buffer; } */
static std::string AnsiString(uint32_t s) {
	if (!s)
		return {};
	uint16_t len = M().U16(s);
	uint32_t buf = M().U32(s + 4);
	std::string out;
	for (uint16_t i = 0; i < len && buf; i++)
		out += (char)M().U8(buf + i);
	return out;
}

/* X_OBJECT_ATTRIBUTES { u32 RootDirectory; u32 ObjectName (ANSI_STRING *); u32 Attributes; } */
static std::string ObjectName(uint32_t attr) {
	if (!attr)
		return {};
	std::string name = AnsiString(M().U32(attr + 4));
	uint32_t root = M().U32(attr);
	if (root && root != 0xFFFFFFFD) {
		auto obj = G()->GetHandle(root);
		if (obj && obj->type == KObject::K_FILE) {
			std::string dir = static_cast<KFileObject *>(obj.get())->path;
			if (!dir.empty() && dir.back() != '\\')
				dir += '\\';
			name = dir + name;
		}
	}
	return name;
}

/* Timeouts: null = forever, negative = relative 100 ns units, positive =
 * absolute system time. Returns the deadline, or max() for forever. */
static std::chrono::steady_clock::time_point Deadline(uint32_t timeout_ptr) {
	using namespace std::chrono;
	if (!timeout_ptr)
		return steady_clock::time_point::max();
	int64_t t = (int64_t)M().U64(timeout_ptr);
	if (t <= 0)
		return steady_clock::now() + microseconds(-t / 10);
	/* absolute FILETIME */
	int64_t now = (int64_t)(duration_cast<microseconds>(system_clock::now().time_since_epoch()).count() * 10 + 116444736000000000ll);
	return steady_clock::now() + microseconds(std::max<int64_t>(0, t - now) / 10);
}

static uint64_t FileTimeNow() {
	using namespace std::chrono;
	return (uint64_t)(duration_cast<microseconds>(system_clock::now().time_since_epoch()).count() * 10 + 116444736000000000ll);
}

static uint32_t CurrentKThread() { return tls_current_thread->kthread; }

/* ---- thread state ------------------------------------------------------------------- */

void ThreadSetup(Guest *g, Thread *t) {
	/* KPCR and KTHREAD, the parts the modules read: PCR+0x100 is the
	 * current thread, KTHREAD+0x14C the thread id, +0x160 the last error. */
	t->pcr = g->pool.Alloc(0x1000, 0x1000);
	t->kthread = g->pool.Alloc(0x1000, 0x10);
	Memory &m = g->memory;
	m.W32(t->pcr + 0x000, 0);								/* TLS pointer */
	m.W32(t->pcr + 0x030, t->pcr);							/* self */
	m.W32(t->pcr + 0x070, t->stack_base + t->stack_size);	/* stack base (top) */
	m.W32(t->pcr + 0x074, t->stack_base);					/* stack limit */
	m.W32(t->pcr + 0x100, t->kthread);						/* current thread */
	m.W8(t->pcr + 0x10C, 0);								/* current processor */
	m.W8(t->kthread + 0x000, 6);							/* dispatcher type: thread */
	m.W32(t->kthread + 0x05C, t->stack_base + t->stack_size);
	m.W32(t->kthread + 0x060, t->stack_base);
	m.W32(t->kthread + 0x14C, t->thread_id);
	m.W32(t->kthread + 0x160, 0);
	t->ctx->r13.u64 = t->pcr;
	t->ctx->r1.u64 = t->stack_base + t->stack_size - 0x100;
}

uint32_t CopyXexHeader(Guest *g, const void *data, size_t size) {
	/* A copy of a XEX file header in guest memory, or (no data) a minimal
	 * one with no optional headers, for RtlImageXexHeaderField. */
	Memory &m = g->memory;
	if (!data || size < 0x18) {
		uint32_t hdr = g->pool.Alloc(0x20);
		m.W32(hdr + 0x00, 0x58455832);	/* 'XEX2' */
		m.W32(hdr + 0x14, 0);			/* optional header count */
		return hdr;
	}
	uint32_t hdr = g->pool.Alloc((uint32_t)size, 0x1000);
	if (hdr)
		memcpy(m.Ptr(hdr), data, size);
	return hdr;
}

uint32_t MakeLdrEntry(Guest *g, uint32_t image_base, uint32_t image_size, uint32_t entry, const char *name,
	const void *xex_header, size_t xex_header_size) {
	/* X_LDR_DATA_TABLE_ENTRY: the fields a module may read */
	uint32_t e = g->pool.Alloc(0x100);
	uint32_t hdr = CopyXexHeader(g, xex_header, xex_header_size);
	Memory &m = g->memory;
	for (int i = 0; i < 3; i++) {	/* empty list links point at themselves */
		m.W32(e + 8 * i, e + 8 * i);
		m.W32(e + 8 * i + 4, e + 8 * i);
	}
	m.W32(e + 0x18, image_base);
	m.W32(e + 0x1C, image_base);
	m.W32(e + 0x20, image_size);
	size_t n = name ? strlen(name) : 0;
	uint32_t wname = g->pool.Alloc((uint32_t)(2 * n + 2));
	for (size_t i = 0; i < n; i++)
		m.W16(wname + 2 * (uint32_t)i, (uint8_t)name[i]);
	for (int k = 0; k < 2; k++) {	/* FullDllName, BaseDllName: UNICODE_STRING */
		m.W16(e + 0x24 + 8 * k, (uint16_t)(2 * n));
		m.W16(e + 0x26 + 8 * k, (uint16_t)(2 * n + 2));
		m.W32(e + 0x28 + 8 * k, wname);
	}
	m.W32(e + 0x38, image_size);
	m.W32(e + 0x3C, entry);
	m.W16(e + 0x40, 1);
	m.W32(e + 0x58, hdr);
	return e;
}

void KernelInit(Guest *g) {
	const char *trace = getenv("QAA_TRACE_KERNEL");
	g->trace_kernel = trace && *trace && *trace != '0';
	Memory &m = g->memory;
	/* XboxHardwareInfo { u32 Flags; u8 NumberOfProcessors; u8 PCIBridgeRevisionID;
	 * u8 Reserved[6]; u16 BldrMagic; u16 BldrFlags; } */
	g->var_hardware_info = g->pool.Alloc(0x10);
	m.W32(g->var_hardware_info + 0, 0x00000020);	/* hard disk present */
	m.W8(g->var_hardware_info + 4, 6);				/* hardware threads */
	g->var_debug_monitor = g->pool.Alloc(0x10);		/* no debugger: zero */
	g->var_thread_type = g->pool.Alloc(0x40);		/* OBJECT_TYPE: contents unused */
	g->start_time = std::chrono::steady_clock::now();
	g->var_time_stamp = g->pool.Alloc(0x20);
	m.W32(g->var_thread_type + 0x10, 0x54687264);	/* pool tag 'Thrd' */
	/* the executable's module handle: default.xex is not loaded, so its
	 * header has no optional headers and the modules use their defaults */
	g->exe_ldr_entry = MakeLdrEntry(g, 0x82000000, 0, 0, "default.xex", nullptr, 0);
	g->exe_xex_header = m.U32(g->exe_ldr_entry + 0x58);
	g->var_exe_module = g->pool.Alloc(0x10);
	m.W32(g->var_exe_module, g->exe_ldr_entry);
	/* KeQueryPerformanceFrequency must match mftb, which the recompiler
	 * implements with the host's rdtsc */
	using namespace std::chrono;
	auto t0 = steady_clock::now();
	uint64_t c0 = __rdtsc();
	std::this_thread::sleep_for(milliseconds(20));
	uint64_t c1 = __rdtsc();
	double s = duration<double>(steady_clock::now() - t0).count();
	g->tsc_frequency = s > 0 ? (double)(c1 - c0) / s : 3.0e9;
}

uint32_t KernelVariable(Guest *g, const char *library, uint32_t ordinal, const char *name) {
	if (strcmp(library, "xboxkrnl.exe"))
		return 0;
	switch (ordinal) {
	case ORD_XboxHardwareInfo: return g->var_hardware_info;
	case ORD_KeDebugMonitorData: return g->var_debug_monitor;
	case ORD_ExThreadObjectType: return g->var_thread_type;
	case ORD_XexExecutableModuleHandle: return g->var_exe_module;
	case ORD_ExEventObjectType: {
		std::lock_guard<std::mutex> l(g->var_lock);
		if (!g->var_event_type) {
			g->var_event_type = g->pool.Alloc(0x40);
			g->memory.W32(g->var_event_type + 0x10, 0x76657645);	/* pool tag 'Even' */
		}
		return g->var_event_type;
	}
	case ORD_KeTimeStampBundle:
		UpdateTimeStamp(g);
		return g->var_time_stamp;
	case ORD_XboxKrnlVersion: {
		std::lock_guard<std::mutex> l(g->var_lock);
		if (!g->var_krnl_version) {
			/* { u16 Major, Minor, Build, Qfe }: a late retail kernel */
			g->var_krnl_version = g->pool.Alloc(8);
			g->memory.W16(g->var_krnl_version + 0, 2);
			g->memory.W16(g->var_krnl_version + 4, 17559);
		}
		return g->var_krnl_version;
	}
	default: {
		/* The title executable imports variables that mean nothing on the
		 * PC (the GPU device, ExLoadedCommandLine, certificate data): a
		 * zeroed block, so code that reads one finds empty data. */
		std::lock_guard<std::mutex> l(g->var_lock);
		uint32_t &v = g->var_blank[ordinal];
		if (!v)
			v = g->pool.Alloc(0x100);
		return v;
	}
	}
}

void UpdateTimeStamp(Guest *g) {
	if (!g->var_time_stamp)
		return;
	/* KTIME_STAMP_BUNDLE { u64 InterruptTime; u64 SystemTime; u32 TickCount; }:
	 * the guest reads it directly (GetTickCount) */
	using namespace std::chrono;
	uint64_t ms = (uint64_t)duration_cast<milliseconds>(steady_clock::now() - g->start_time).count();
	g->memory.W64(g->var_time_stamp + 0x00, ms * 10000);
	g->memory.W64(g->var_time_stamp + 0x08, ms * 10000);
	g->memory.W32(g->var_time_stamp + 0x10, (uint32_t)ms);
}

/* ---- waits ------------------------------------------------------------------- */

static bool IsWaitable(const std::shared_ptr<KObject> &o) {
	return o && (o->type == KObject::K_EVENT || o->type == KObject::K_THREAD);
}

static uint32_t WaitObjectsLocked(Guest *g, std::unique_lock<std::mutex> &l, std::vector<std::shared_ptr<KObject>> &objs,
	bool wait_all, std::chrono::steady_clock::time_point deadline);

static uint32_t WaitObjects(std::vector<std::shared_ptr<KObject>> &objs, bool wait_all, uint32_t timeout_ptr) {
	Guest *g = G();
	auto deadline = Deadline(timeout_ptr);
	std::unique_lock<std::mutex> l(g->wait_lock);
	/* qaa_guest_step_thread watches what each thread waits on */
	Thread *t = tls_current_thread;
	t->blocked_on = objs.empty() ? nullptr : objs[0].get();
	t->waits_begun++;
	g->wait_cv.notify_all();
	uint32_t r = WaitObjectsLocked(g, l, objs, wait_all, deadline);
	t->blocked_on = nullptr;
	return r;
}

static uint32_t WaitObjectsLocked(Guest *g, std::unique_lock<std::mutex> &l, std::vector<std::shared_ptr<KObject>> &objs,
	bool wait_all, std::chrono::steady_clock::time_point deadline) {
	for (;;) {
		if (wait_all) {
			bool all = std::all_of(objs.begin(), objs.end(), [](auto &o) { return o->signaled; });
			if (all) {
				for (auto &o : objs)
					if (!o->manual_reset)
						o->signaled = false;
				return STATUS_SUCCESS;
			}
		} else {
			for (size_t i = 0; i < objs.size(); i++) {
				if (objs[i]->signaled) {
					if (!objs[i]->manual_reset)
						objs[i]->signaled = false;
					return STATUS_SUCCESS + (uint32_t)i;
				}
			}
		}
		if (deadline == std::chrono::steady_clock::time_point::max())
			g->wait_cv.wait(l);
		else if (g->wait_cv.wait_until(l, deadline) == std::cv_status::timeout && std::chrono::steady_clock::now() >= deadline)
			return STATUS_TIMEOUT;
	}
}

static std::shared_ptr<KObject> CurrentThreadObject() {
	Thread *t = tls_current_thread;
	return t->object;
}

/* ---- files ----------------------------------------------------------------------- */

enum : uint32_t {
	FILE_SUPERSEDE = 0, FILE_OPEN = 1, FILE_CREATE = 2, FILE_OPEN_IF = 3, FILE_OVERWRITE = 4, FILE_OVERWRITE_IF = 5,
	FILE_DIRECTORY_FILE = 0x1, FILE_NON_DIRECTORY_FILE = 0x40,
	FILE_OPENED = 1, FILE_CREATED = 2, FILE_OVERWRITTEN = 3, FILE_DOES_NOT_EXIST = 5,
	GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000, GENERIC_ALL = 0x10000000,
	FILE_READ_DATA = 0x1, FILE_WRITE_DATA = 0x2, FILE_APPEND_DATA = 0x4,
	FILE_ATTRIBUTE_DIRECTORY = 0x10, FILE_ATTRIBUTE_NORMAL = 0x80,
};

static uint32_t OpenFile(PPCContext &ctx, uint32_t handle_ptr, uint32_t access, uint32_t attr, uint32_t iosb,
	uint32_t disposition, uint32_t options) {
	Guest *g = G();
	std::string path = ObjectName(attr);
	int host_access = 0;
	if (access & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA))
		host_access |= QAA_FILE_READ;
	if (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA))
		host_access |= QAA_FILE_WRITE;
	qaa_file_info info = {};
	bool exists = g->host.file_stat && g->host.file_stat(g->host.user, path.c_str(), &info) == 0;
	auto obj = std::make_shared<KFileObject>();
	obj->path = path;
	uint32_t result_info = FILE_OPENED;
	if (exists && info.is_directory) {
		if (options & FILE_NON_DIRECTORY_FILE) {
			IoStatus(iosb, STATUS_FILE_IS_A_DIRECTORY, 0);
			return STATUS_FILE_IS_A_DIRECTORY;
		}
		obj->is_directory = true;
	} else {
		if (options & FILE_DIRECTORY_FILE) {
			uint32_t st = exists ? STATUS_NOT_A_DIRECTORY : STATUS_OBJECT_NAME_NOT_FOUND;
			IoStatus(iosb, st, 0);
			return st;
		}
		if (!exists && (disposition == FILE_OPEN || disposition == FILE_OVERWRITE)) {
			IoStatus(iosb, STATUS_OBJECT_NAME_NOT_FOUND, FILE_DOES_NOT_EXIST);
			return STATUS_OBJECT_NAME_NOT_FOUND;
		}
		if (exists && disposition == FILE_CREATE) {
			IoStatus(iosb, STATUS_OBJECT_NAME_COLLISION, 0);
			return STATUS_OBJECT_NAME_COLLISION;
		}
		int host_disp = (int)disposition;
		if (!g->host.file_open) {
			IoStatus(iosb, STATUS_OBJECT_NAME_NOT_FOUND, FILE_DOES_NOT_EXIST);
			return STATUS_OBJECT_NAME_NOT_FOUND;
		}
		obj->host_handle = g->host.file_open(g->host.user, path.c_str(), host_access, host_disp);
		if (obj->host_handle < 0) {
			uint32_t st = (host_access & QAA_FILE_WRITE) ? STATUS_ACCESS_DENIED : STATUS_OBJECT_NAME_NOT_FOUND;
			IoStatus(iosb, st, 0);
			return st;
		}
		result_info = !exists ? FILE_CREATED : (disposition >= FILE_OVERWRITE || disposition == FILE_SUPERSEDE) ? FILE_OVERWRITTEN : FILE_OPENED;
	}
	uint32_t h = g->AddHandle(obj);
	if (handle_ptr)
		M().W32(handle_ptr, h);
	IoStatus(iosb, STATUS_SUCCESS, result_info);
	if (g->trace_kernel)
		g->Log(QAA_LOG_DEBUG, "kernel: open \"%s\" access %08X disposition %u -> handle %08X", path.c_str(), access, disposition, h);
	return STATUS_SUCCESS;
}

static std::shared_ptr<KFileObject> FileHandle(uint32_t h) {
	auto o = G()->GetHandle(h);
	if (!o || o->type != KObject::K_FILE)
		return nullptr;
	return std::static_pointer_cast<KFileObject>(o);
}

static int64_t FileSize(KFileObject *f) {
	Guest *g = G();
	if (f->is_directory || f->host_handle < 0 || !g->host.file_size)
		return 0;
	return g->host.file_size(g->host.user, f->host_handle);
}

static void SignalEvent(uint32_t handle) {
	if (!handle)
		return;
	auto o = G()->GetHandle(handle);
	if (o && o->type == KObject::K_EVENT) {
		{
			std::lock_guard<std::mutex> l(G()->wait_lock);
			o->signaled = true;
		}
		G()->wait_cv.notify_all();
	}
}

static uint32_t ReadWrite(PPCContext &ctx, bool write) {
	Guest *g = G();
	uint32_t handle = ctx.r3.u32, event = ctx.r4.u32, apc = ctx.r5.u32, iosb = ctx.r7.u32;
	uint32_t buffer = ctx.r8.u32, length = ctx.r9.u32, offset_ptr = ctx.r10.u32;
	auto f = FileHandle(handle);
	if (!f) {
		IoStatus(iosb, STATUS_INVALID_HANDLE, 0);
		return STATUS_INVALID_HANDLE;
	}
	if (apc)
		g->Log(QAA_LOG_WARNING, "kernel: %s with an APC routine (%08X): not called", write ? "NtWriteFile" : "NtReadFile", apc);
	int64_t offset = f->position;
	if (offset_ptr) {
		int64_t o = (int64_t)M().U64(offset_ptr);
		if (o >= 0)
			offset = o;
		else if (o == -1 && write)	/* FILE_WRITE_TO_END_OF_FILE */
			offset = FileSize(f.get());
	}
	if (!M().IsCommitted(buffer, length)) {
		IoStatus(iosb, STATUS_ACCESS_DENIED, 0);
		return STATUS_ACCESS_DENIED;
	}
	int64_t n = -1;
	if (f->host_handle >= 0) {
		if (write && g->host.file_write)
			n = g->host.file_write(g->host.user, f->host_handle, offset, M().Ptr(buffer), length);
		else if (!write && g->host.file_read)
			n = g->host.file_read(g->host.user, f->host_handle, offset, M().Ptr(buffer), length);
	}
	if (n < 0) {
		IoStatus(iosb, STATUS_UNSUCCESSFUL, 0);
		return STATUS_UNSUCCESSFUL;
	}
	f->position = offset + n;
	uint32_t st = (!write && n == 0 && length) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
	IoStatus(iosb, st, (uint32_t)n);
	SignalEvent(event);
	return st;
}

/* FILE_NETWORK_OPEN_INFORMATION: 4 times, AllocationSize, EndOfFile, FileAttributes */
static void WriteNetworkOpenInfo(uint32_t p, int64_t size, bool dir) {
	uint64_t now = FileTimeNow();
	for (int i = 0; i < 4; i++)
		M().W64(p + 8 * i, now);
	M().W64(p + 0x20, (uint64_t)((size + 0xFFF) & ~0xFFFll));
	M().W64(p + 0x28, (uint64_t)size);
	M().W32(p + 0x30, dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
	M().W32(p + 0x34, 0);
}

static uint32_t QueryInformationFile(PPCContext &ctx) {
	uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, length = ctx.r6.u32, cls = ctx.r7.u32;
	auto f = FileHandle(ctx.r3.u32);
	if (!f)
		return STATUS_INVALID_HANDLE;
	int64_t size = FileSize(f.get());
	uint32_t need = 0;
	switch (cls) {
	case 4:		/* FileBasicInformation */
		need = 0x28;
		if (length < need) break;
		for (int i = 0; i < 4; i++)
			M().W64(info + 8 * i, FileTimeNow());
		M().W32(info + 0x20, f->is_directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
		M().W32(info + 0x24, 0);
		break;
	case 5:		/* FileStandardInformation */
		need = 0x18;
		if (length < need) break;
		M().W64(info + 0, (uint64_t)((size + 0xFFF) & ~0xFFFll));
		M().W64(info + 8, (uint64_t)size);
		M().W32(info + 0x10, 1);
		M().W8(info + 0x14, 0);
		M().W8(info + 0x15, f->is_directory);
		M().W16(info + 0x16, 0);
		break;
	case 6:		/* FileInternalInformation */
		need = 8;
		if (length < need) break;
		M().W64(info, ctx.r3.u32);
		break;
	case 14:	/* FilePositionInformation */
		need = 8;
		if (length < need) break;
		M().W64(info, (uint64_t)f->position);
		break;
	case 20:	/* FileEndOfFileInformation */
		need = 8;
		if (length < need) break;
		M().W64(info, (uint64_t)size);
		break;
	case 34:	/* FileNetworkOpenInformation */
		need = 0x38;
		if (length < need) break;
		WriteNetworkOpenInfo(info, size, f->is_directory);
		break;
	default:
		G()->Log(QAA_LOG_WARNING, "kernel: NtQueryInformationFile class %u not implemented", cls);
		IoStatus(iosb, STATUS_NOT_IMPLEMENTED, 0);
		return STATUS_NOT_IMPLEMENTED;
	}
	if (length < need) {
		IoStatus(iosb, STATUS_INFO_LENGTH_MISMATCH, 0);
		return STATUS_INFO_LENGTH_MISMATCH;
	}
	IoStatus(iosb, STATUS_SUCCESS, need);
	return STATUS_SUCCESS;
}

static uint32_t SetInformationFile(PPCContext &ctx) {
	uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, cls = ctx.r7.u32;
	auto f = FileHandle(ctx.r3.u32);
	if (!f)
		return STATUS_INVALID_HANDLE;
	switch (cls) {
	case 14:	/* FilePositionInformation */
		f->position = (int64_t)M().U64(info);
		break;
	case 20: {	/* FileEndOfFileInformation: resize through the host */
		Guest *g = G();
		int64_t size = (int64_t)M().U64(info);
		if (f->host_handle < 0 || !g->host.file_set_size || g->host.file_set_size(g->host.user, f->host_handle, size) != 0) {
			IoStatus(iosb, STATUS_ACCESS_DENIED, 0);
			return STATUS_ACCESS_DENIED;
		}
		break;
	}
	case 4:		/* FileBasicInformation: times and attributes, ignored */
	case 13:	/* FileDispositionInformation: delete on close, ignored */
	case 19:	/* FileAllocationInformation: a reservation hint, ignored */
		G()->Log(QAA_LOG_DEBUG, "kernel: NtSetInformationFile class %u ignored for \"%s\"", cls, f->path.c_str());
		break;
	default:
		G()->Log(QAA_LOG_WARNING, "kernel: NtSetInformationFile class %u not implemented", cls);
		IoStatus(iosb, STATUS_NOT_IMPLEMENTED, 0);
		return STATUS_NOT_IMPLEMENTED;
	}
	IoStatus(iosb, STATUS_SUCCESS, 0);
	return STATUS_SUCCESS;
}

/* Case-insensitive wildcard match with * and ?. */
static bool WildMatch(const char *p, const char *s) {
	for (; *p; p++, s++) {
		if (*p == '*') {
			while (p[1] == '*')
				p++;
			if (!p[1])
				return true;
			for (; *s; s++)
				if (WildMatch(p + 1, s))
					return true;
			return false;
		}
		if (!*s)
			return false;
		if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s))
			return false;
	}
	return !*s;
}

/* One X_FILE_DIRECTORY_INFORMATION per call:
 *   0x00 NextEntryOffset, 0x04 FileIndex, 0x08..0x27 four times,
 *   0x28 EndOfFile, 0x30 AllocationSize, 0x38 FileAttributes,
 *   0x3C FileNameLength, 0x40 FileName (ANSI, not terminated) */
static uint32_t QueryDirectoryFile(PPCContext &ctx) {
	Guest *g = G();
	uint32_t event = ctx.r4.u32, iosb = ctx.r7.u32, info = ctx.r8.u32, length = ctx.r9.u32;
	uint32_t pattern = ctx.r10.u32;
	bool restart = StackArg(ctx, 0) != 0;
	auto f = FileHandle(ctx.r3.u32);
	if (!f || !f->is_directory) {
		IoStatus(iosb, STATUS_INVALID_HANDLE, 0);
		return STATUS_INVALID_HANDLE;
	}
	if (restart || f->dir_pattern.empty()) {
		f->dir_index = 0;
		std::string p = pattern ? AnsiString(pattern) : std::string();
		f->dir_pattern = p.empty() ? "*" : p;
	}
	bool first = f->dir_index == 0;
	for (;;) {
		char name[260];
		qaa_file_info fi = {};
		if (!g->host.dir_entry || g->host.dir_entry(g->host.user, f->path.c_str(), f->dir_index, name, sizeof(name), &fi) != 0) {
			uint32_t st = first ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES;
			IoStatus(iosb, st, 0);
			SignalEvent(event);
			return st;
		}
		f->dir_index++;
		name[sizeof(name) - 1] = 0;
		if (!WildMatch(f->dir_pattern.c_str(), name))
			continue;
		uint32_t n = (uint32_t)strlen(name);
		if (length < 0x40 + n) {
			f->dir_index--;
			IoStatus(iosb, STATUS_BUFFER_TOO_SMALL, 0);
			return STATUS_BUFFER_TOO_SMALL;
		}
		Memory &m = M();
		uint64_t now = FileTimeNow();
		m.W32(info + 0x00, 0);
		m.W32(info + 0x04, f->dir_index - 1);
		for (int i = 0; i < 4; i++)
			m.W64(info + 0x08 + 8 * i, now);
		m.W64(info + 0x28, (uint64_t)fi.size);
		m.W64(info + 0x30, (uint64_t)((fi.size + 0xFFF) & ~0xFFFll));
		m.W32(info + 0x38, fi.is_directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
		m.W32(info + 0x3C, n);
		memcpy(m.Ptr(info + 0x40), name, n);
		IoStatus(iosb, STATUS_SUCCESS, 0x40 + n);
		SignalEvent(event);
		return STATUS_SUCCESS;
	}
}

/* ---- critical sections ------------------------------------------------------------------ */
/* X_RTL_CRITICAL_SECTION: DISPATCHER_HEADER (0x10), LockCount s32 @0x10,
 * RecursionCount s32 @0x14, OwningThread (KTHREAD *) @0x18. */

static void CsInit(uint32_t cs) {
	Memory &m = M();
	m.W8(cs + 0, 1);		/* SynchronizationEvent */
	m.W8(cs + 2, 4);
	m.W32(cs + 4, 0);
	m.W32(cs + 8, cs + 8);
	m.W32(cs + 12, cs + 8);
	m.W32(cs + 0x10, 0xFFFFFFFF);
	m.W32(cs + 0x14, 0);
	m.W32(cs + 0x18, 0);
}

static bool CsEnter(uint32_t cs, bool try_only) {
	Guest *g = G();
	Memory &m = g->memory;
	uint32_t me = CurrentKThread();
	std::unique_lock<std::mutex> l(g->cs_lock);
	for (;;) {
		uint32_t owner = m.U32(cs + 0x18);
		if (owner == 0 || owner == me) {
			m.W32(cs + 0x18, me);
			m.W32(cs + 0x14, m.U32(cs + 0x14) + 1);
			m.W32(cs + 0x10, m.U32(cs + 0x10) + 1);
			return true;
		}
		if (try_only)
			return false;
		g->cs_cv.wait(l);
	}
}

static void CsLeave(uint32_t cs) {
	Guest *g = G();
	Memory &m = g->memory;
	bool wake = false;
	{
		std::lock_guard<std::mutex> l(g->cs_lock);
		if (m.U32(cs + 0x18) != CurrentKThread()) {
			g->Log(QAA_LOG_WARNING, "kernel: RtlLeaveCriticalSection(%08X) by a thread that does not own it", cs);
			return;
		}
		uint32_t rec = m.U32(cs + 0x14) - 1;
		m.W32(cs + 0x14, rec);
		m.W32(cs + 0x10, m.U32(cs + 0x10) - 1);
		if (!rec) {
			m.W32(cs + 0x18, 0);
			wake = true;
		}
	}
	if (wake)
		g->cs_cv.notify_all();
}

/* ---- guest exceptions ------------------------------------------------------------------- */
/*
 * RtlRaiseException in two phases, as the Xbox kernel's dispatcher does,
 * over the guest stack:
 *
 *  search  Walk the frames from the raise: each frame's return address is
 *          the LR its callee saved at (caller SP - 8), the caller SP its
 *          back chain at 0(SP). For every __except scope around a frame's
 *          call, innermost first, run the filter funclet (r3 = the
 *          EXCEPTION_POINTERS, r12 = the frame's caller SP). EXECUTE_HANDLER
 *          picks the handler; CONTINUE_EXECUTION returns from the raise.
 *  unwind  Run the __finally funclets of the frames being left (and of the
 *          handler frame's scopes inside the chosen one), restore the
 *          non-volatile registers their prologues saved, and set r1 to the
 *          handler frame's. The recompiled code then leaves each unwound
 *          frame and resumes at the handler label through the
 *          PPC_SEH_RESUME checks after its calls (SehResume).
 *
 * A raise no filter accepts is unhandled: the guest call fails, as the
 * title would crash on the console.
 */

static const SehFunction *FindSehFunction(Guest *g, uint32_t a, const ModuleDesc **mod) {
	for (auto &m : g->modules) {
		const ModuleDesc *d = m->desc;
		for (uint32_t i = 0; i < d->seh_function_count; i++) {
			const SehFunction &f = d->seh_functions[i];
			if (f.begin <= a && a < f.end) {
				*mod = d;
				return &f;
			}
		}
	}
	return nullptr;
}

static const UnwindFunction *FindUnwind(Guest *g, uint32_t a, const ModuleDesc **mod) {
	for (auto &m : g->modules) {
		const ModuleDesc *d = m->desc;
		for (uint32_t i = 0; i < d->unwind_function_count; i++) {
			const UnwindFunction &f = d->unwind_functions[i];
			if (f.begin <= a && a < f.end) {
				*mod = d;
				return &f;
			}
		}
	}
	return nullptr;
}

static bool FindSite(Guest *g, uint32_t site, uint32_t *target) {
	for (auto &m : g->modules) {
		const ModuleDesc *d = m->desc;
		for (uint32_t i = 0; i < d->seh_site_count; i++) {
			if (d->seh_sites[i].site == site) {
				*target = d->seh_sites[i].target;
				return true;
			}
		}
	}
	return false;
}

static int32_t CallFunclet(Guest *g, uint32_t fn, int64_t r3, uint32_t establisher, const char *what) {
	int64_t result = 0;
	if (CallGuestR12(g, fn, &r3, 1, &result, establisher))
		Fatal("SEH %s funclet %08X failed: %s", what, fn, g->last_error.c_str());
	return (int32_t)result;
}

void RaiseGuestException(PPCContext &ctx, uint32_t record) {
	Thread *t = tls_current_thread;
	Guest *g = t->guest;
	Memory &m = g->memory;
	const uint32_t code = m.U32(record), flags = m.U32(record + 4);
	if (g->trace_kernel)
		g->Log(QAA_LOG_DEBUG, "kernel: guest exception %08X raised from %08X", code, (uint32_t)ctx.lr - 4);

	/* EXCEPTION_POINTERS { ExceptionRecord, ContextRecord }; the context is
	 * left empty (no filter in the modules reads it). */
	uint32_t ptrs = g->pool.Alloc(8 + 0x200);
	m.W32(ptrs, record);
	m.W32(ptrs + 4, ptrs + 8);

	struct Frame {
		uint32_t sp, site;
	};
	Frame frames[64];
	int n = 0, found = -1;
	SehScope hit = {};
	uint32_t sp = ctx.r1.u32, ra = (uint32_t)ctx.lr;
	for (; n < 64 && found < 0; n++) {
		frames[n] = { sp, ra - 4 };
		const ModuleDesc *d;
		if (const SehFunction *f = FindSehFunction(g, ra - 4, &d)) {
			for (uint32_t i = 0; i < f->scope_count; i++) {
				const SehScope &sc = d->seh_scopes[f->first_scope + i];
				if (!sc.target || ra - 4 < sc.begin || ra - 4 >= sc.end)
					continue;
				int32_t verdict = sc.handler < 0x10000 ? (int32_t)sc.handler
					: CallFunclet(g, sc.handler, ptrs, m.U32(sp), "filter");
				if (verdict > 0) {	/* EXCEPTION_EXECUTE_HANDLER */
					found = n;
					hit = sc;
					break;
				}
				if (verdict < 0) {	/* EXCEPTION_CONTINUE_EXECUTION */
					g->pool.Free(ptrs);
					if (flags & 1)	/* EXCEPTION_NONCONTINUABLE */
						Fatal("guest exception %08X is noncontinuable, but a filter continued it", code);
					return;
				}
			}
		}
		if (found >= 0)
			break;
		uint32_t caller = m.U32(sp);
		if (!caller || caller <= sp || !m.IsCommitted(caller - 8, 8))
			break;
		ra = m.U32(caller - 8);
		sp = caller;
		if (!ra)
			break;
	}
	g->pool.Free(ptrs);
	if (found < 0)
		Fatal("unhandled guest exception %08X (raised from %08X): no __except filter accepted it",
			code, frames[0].site);

	/* every frame to leave must be able to leave, and the handler frame resume */
	for (int j = 0; j <= found; j++) {
		uint32_t target = 0;
		const ModuleDesc *d;
		if (!FindSite(g, frames[j].site, &target))
			Fatal("guest exception %08X: the call at %08X has no SEH resume site", code, frames[j].site);
		if (j < found && !FindUnwind(g, frames[j].site, &d))
			Fatal("guest exception %08X: no unwind data for the frame calling at %08X", code, frames[j].site);
		if (j == found && target != hit.target)
			Fatal("guest exception %08X: the handler %08X is not the innermost at %08X", code, hit.target, frames[j].site);
	}

	/* termination handlers, innermost first */
	for (int j = 0; j <= found; j++) {
		const ModuleDesc *d;
		const SehFunction *f = FindSehFunction(g, frames[j].site, &d);
		if (!f)
			continue;
		for (uint32_t i = 0; i < f->scope_count; i++) {
			const SehScope &sc = d->seh_scopes[f->first_scope + i];
			if (sc.target || frames[j].site < sc.begin || frames[j].site >= sc.end)
				continue;
			if (j == found && !(hit.begin <= sc.begin && sc.end <= hit.end))
				continue;	/* outside the chosen __try: it stays */
			CallFunclet(g, sc.handler, 1 /* abnormal termination */, m.U32(frames[j].sp), "termination");
		}
	}

	/* pop the frames: restore what each prologue saved, innermost first,
	 * so the outer frames' saved values (the handler frame's) win */
	for (int j = 0; j < found; j++) {
		const ModuleDesc *d;
		const UnwindFunction *u = FindUnwind(g, frames[j].site, &d);
		uint32_t caller = m.U32(frames[j].sp);
		for (uint32_t i = 0; i < u->save_count; i++) {
			const UnwindSave &s = d->unwind_saves[u->first_save + i];
			uint64_t v = m.U64(caller + (uint32_t)s.offset);
			if (s.kind == UNWIND_GPR && s.reg >= 14 && s.reg <= 31)
				(&ctx.r14)[s.reg - 14].u64 = v;
			else if (s.kind == UNWIND_FPR && s.reg >= 14 && s.reg <= 31)
				(&ctx.f14)[s.reg - 14].u64 = v;
		}
	}
	ctx.r1.u64 = frames[found].sp;
	t->seh.active = true;
	t->seh.frames_left = found;
	t->seh.target = hit.target;
	if (g->trace_kernel)
		g->Log(QAA_LOG_DEBUG, "kernel: guest exception %08X: handler %08X, %d frame(s) unwound", code, hit.target, found);
}

uint32_t SehResume(PPCContext &, uint8_t *, uint32_t site) {
	Thread *t = tls_current_thread;
	if (!t || !t->seh.active)
		return 0;
	if (t->seh.frames_left > 0) {
		t->seh.frames_left--;
		return 1;
	}
	uint32_t target = 0;
	if (!FindSite(t->guest, site, &target) || target != t->seh.target)
		Fatal("SEH resume at %08X does not lead to the handler %08X", site, t->seh.target);
	t->seh.active = false;
	return 2;
}

/* ---- the dispatcher ---------------------------------------------------------------------- */

/* An import the runtime does not implement: the host's import hook, if any. */
static bool RunImportHook(Guest *g, uint32_t library, uint32_t ordinal, PPCContext &ctx) {
	Hook h;
	{
		std::lock_guard<std::mutex> l(g->hook_lock);
		auto it = g->import_hooks.find(library << 16 | ordinal);
		h = it != g->import_hooks.end() ? it->second : g->default_import_hook;
	}
	if (!h.fn)
		return false;
	RunHook(g, h, (library << 16) | ordinal, ctx);
	return true;
}

void KernelCall(uint32_t library, uint32_t ordinal, PPCContext &ctx, uint8_t *base) {
	Thread *t = tls_current_thread;
	if (!t)
		abort();	/* a module called the kernel outside a guest call */
	Guest *g = t->guest;
	UpdateTimeStamp(g);
	Memory &m = g->memory;
	if (library != 0) {
		if (RunImportHook(g, library, ordinal, ctx))
			return;
		Fatal("call to unimplemented %s ordinal %u", "xam.xex", ordinal);
	}
	if (g->trace_kernel)
		g->Log(QAA_LOG_DEBUG, "kernel: %s(%08X, %08X, %08X, %08X) lr %08X", KernelName(ordinal) ? KernelName(ordinal) : "?",
			ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, (uint32_t)ctx.lr);
	uint64_t r = 0;
	switch (ordinal) {

	/* -- threads ------------------------------------------------------------------- */
	case 0x00D: {	/* ExCreateThread(handle*, stack_size, thread_id*, xapi_startup, start, context, flags) */
		uint32_t handle_ptr = ctx.r3.u32, stack = ctx.r4.u32, id_ptr = ctx.r5.u32;
		auto obj = std::make_shared<KThreadObject>();
		obj->xapi_startup = ctx.r6.u32;
		obj->start_address = ctx.r7.u32;
		obj->start_context = ctx.r8.u32;
		uint32_t flags = ctx.r9.u32;
		obj->suspend_count = (flags & 1) ? 1 : 0;	/* CREATE_SUSPENDED */
		Thread *nt = g->NewThread(std::max<uint32_t>(stack, 0x40000));
		if (!nt) {
			r = STATUS_NO_MEMORY;
			break;
		}
		nt->guest_created = true;
		nt->object = obj;
		obj->thread = nt;
		obj->kthread = nt->kthread;
		uint32_t h = g->AddHandle(obj);
		if (handle_ptr)
			m.W32(handle_ptr, h);
		if (id_ptr)
			m.W32(id_ptr, nt->thread_id);
		{
			std::lock_guard<std::mutex> l(g->threads_lock);
			g->threads.push_back(obj);
		}
		g->Log(QAA_LOG_INFO, "kernel: ExCreateThread start %08X context %08X startup %08X flags %08X -> thread %u%s",
			obj->start_address, obj->start_context, obj->xapi_startup, flags, nt->thread_id, obj->suspend_count ? " (suspended)" : "");
		StartGuestThread(g, obj);
		r = STATUS_SUCCESS;
		break;
	}
	case 0x019:		/* ExTerminateThread(exit_code) */
		ExitThread(ctx.r3.u32);
	case 0x0F5: {	/* NtResumeThread(handle, suspend_count*) */
		auto o = g->GetHandle(ctx.r3.u32);
		if (!o || o->type != KObject::K_THREAD) {
			r = STATUS_INVALID_HANDLE;
			break;
		}
		auto *th = static_cast<KThreadObject *>(o.get());
		uint32_t prev;
		{
			std::lock_guard<std::mutex> l(g->wait_lock);
			prev = th->suspend_count;
			if (th->suspend_count)
				th->suspend_count--;
		}
		g->wait_cv.notify_all();
		if (ctx.r4.u32)
			m.W32(ctx.r4.u32, prev);
		r = STATUS_SUCCESS;
		break;
	}
	case 0x097:		/* KeSetAffinityThread(kthread, affinity, previous*) */
		if (ctx.r5.u32)
			m.W32(ctx.r5.u32, 0x3F);
		r = STATUS_SUCCESS;
		break;
	case 0x110: {	/* ObReferenceObjectByHandle(handle, object_type, object*) */
		uint32_t h = ctx.r3.u32, out = ctx.r5.u32;
		uint32_t obj = 0;
		if (h == H_CURRENT_THREAD) {
			obj = t->kthread;
		} else {
			auto o = g->GetHandle(h);
			if (o && o->type == KObject::K_THREAD)
				obj = static_cast<KThreadObject *>(o.get())->kthread;
			else if (o)
				obj = h;	/* other objects: an opaque token */
		}
		if (!obj) {
			r = STATUS_INVALID_HANDLE;
			break;
		}
		if (out)
			m.W32(out, obj);
		r = STATUS_SUCCESS;
		break;
	}
	case 0x105:		/* ObDereferenceObject(object) */
		break;

	/* -- TLS ------------------------------------------------------------------------- */
	case 0x152: {	/* KeTlsAlloc() */
		std::lock_guard<std::mutex> l(g->tls_lock);
		r = 0xFFFFFFFF;
		for (uint32_t i = 0; i < TLS_SLOTS; i++) {
			if (!(g->tls_used & (1ull << i))) {
				g->tls_used |= 1ull << i;
				r = i;
				break;
			}
		}
		break;
	}
	case 0x153: {	/* KeTlsFree(index) */
		std::lock_guard<std::mutex> l(g->tls_lock);
		uint32_t i = ctx.r3.u32;
		r = i < TLS_SLOTS && (g->tls_used & (1ull << i));
		if (r)
			g->tls_used &= ~(1ull << i);
		break;
	}
	case 0x154:		/* KeTlsGetValue(index) */
		r = ctx.r3.u32 < TLS_SLOTS ? t->tls[ctx.r3.u32] : 0;
		break;
	case 0x155:		/* KeTlsSetValue(index, value) */
		if (ctx.r3.u32 < TLS_SLOTS) {
			t->tls[ctx.r3.u32] = ctx.r4.u32;
			r = 1;
		}
		break;

	/* -- events and waits ------------------------------------------------------------------ */
	case 0x0D1: {	/* NtCreateEvent(handle*, attributes, type, initial_state) */
		auto o = std::make_shared<KObject>(KObject::K_EVENT);
		o->manual_reset = ctx.r5.u32 == 0;	/* NotificationEvent */
		o->signaled = ctx.r6.u32 != 0;
		uint32_t h = g->AddHandle(o);
		if (ctx.r3.u32)
			m.W32(ctx.r3.u32, h);
		r = STATUS_SUCCESS;
		break;
	}
	case 0x0F6:		/* NtSetEvent(handle, previous_state*) */
	case 0x0CE: {	/* NtClearEvent(handle) */
		auto o = g->GetHandle(ctx.r3.u32);
		if (!o || o->type != KObject::K_EVENT) {
			r = STATUS_INVALID_HANDLE;
			break;
		}
		uint32_t prev;
		{
			std::lock_guard<std::mutex> l(g->wait_lock);
			prev = o->signaled;
			o->signaled = ordinal == 0x0F6;
		}
		if (ordinal == 0x0F6) {
			g->wait_cv.notify_all();
			if (ctx.r4.u32)
				m.W32(ctx.r4.u32, prev);
		}
		r = STATUS_SUCCESS;
		break;
	}
	case 0x0FD: {	/* NtWaitForSingleObjectEx(handle, mode, alertable, timeout*) */
		uint32_t h = ctx.r3.u32;
		std::shared_ptr<KObject> o = h == H_CURRENT_THREAD ? CurrentThreadObject() : g->GetHandle(h);
		if (!IsWaitable(o)) {
			r = STATUS_INVALID_HANDLE;
			break;
		}
		std::vector<std::shared_ptr<KObject>> v{ o };
		r = WaitObjects(v, true, ctx.r6.u32);
		break;
	}
	case 0x0FE: {	/* NtWaitForMultipleObjectsEx(count, handles*, wait_type, mode, alertable, timeout*) */
		uint32_t count = ctx.r3.u32, handles = ctx.r4.u32;
		std::vector<std::shared_ptr<KObject>> v;
		for (uint32_t i = 0; i < count && i < 64; i++) {
			auto o = g->GetHandle(m.U32(handles + 4 * i));
			if (!IsWaitable(o)) {
				v.clear();
				break;
			}
			v.push_back(o);
		}
		if (v.empty()) {
			r = STATUS_INVALID_HANDLE;
			break;
		}
		r = WaitObjects(v, ctx.r5.u32 == 0, ctx.r8.u32);
		break;
	}

	/* -- handles ------------------------------------------------------------------------- */
	case 0x0CF: {	/* NtClose(handle) */
		auto f = FileHandle(ctx.r3.u32);
		if (f && f->host_handle >= 0 && g->host.file_close && f.use_count() <= 2)
			g->host.file_close(g->host.user, f->host_handle);
		r = g->CloseHandle(ctx.r3.u32) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
		break;
	}
	case 0x0DA: {	/* NtDuplicateObject(handle, new_handle*, options) */
		uint32_t h = ctx.r3.u32;
		std::shared_ptr<KObject> o = h == H_CURRENT_THREAD ? CurrentThreadObject() : g->GetHandle(h);
		if (!o) {
			r = STATUS_INVALID_HANDLE;
			break;
		}
		uint32_t nh = g->AddHandle(o);
		if (ctx.r4.u32)
			m.W32(ctx.r4.u32, nh);
		if (ctx.r5.u32 & 1)	/* DUPLICATE_CLOSE_SOURCE */
			g->CloseHandle(h);
		r = STATUS_SUCCESS;
		break;
	}

	/* -- files ---------------------------------------------------------------------------- */
	case 0x0D2:		/* NtCreateFile(handle*, access, attributes, iosb, allocation*, file_attributes,
					 *              share, disposition, options) */
		r = OpenFile(ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32, StackArg(ctx, 0));
		break;
	case 0x0DF:		/* NtOpenFile(handle*, access, attributes, iosb, share, options) */
		r = OpenFile(ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, FILE_OPEN, ctx.r8.u32);
		break;
	case 0x0F0:		/* NtReadFile(handle, event, apc, apc_context, iosb, buffer, length, offset*) */
		r = ReadWrite(ctx, false);
		break;
	case 0x0FF:		/* NtWriteFile(...) */
		r = ReadWrite(ctx, true);
		break;
	case 0x0F1:		/* NtReadFileScatter: not used by the modules' code paths seen so far */
		g->Log(QAA_LOG_WARNING, "kernel: NtReadFileScatter not implemented");
		IoStatus(ctx.r7.u32, STATUS_NOT_IMPLEMENTED, 0);
		r = STATUS_NOT_IMPLEMENTED;
		break;
	case 0x0E8:		/* NtQueryInformationFile(handle, iosb, info, length, class) */
		r = QueryInformationFile(ctx);
		break;
	case 0x0F7:		/* NtSetInformationFile(handle, iosb, info, length, class) */
		r = SetInformationFile(ctx);
		break;
	case 0x0E7: {	/* NtQueryFullAttributesFile(attributes, info*) */
		std::string path = ObjectName(ctx.r3.u32);
		qaa_file_info info = {};
		if (!g->host.file_stat || g->host.file_stat(g->host.user, path.c_str(), &info) != 0) {
			r = STATUS_OBJECT_NAME_NOT_FOUND;
			break;
		}
		WriteNetworkOpenInfo(ctx.r4.u32, info.size, info.is_directory != 0);
		r = STATUS_SUCCESS;
		break;
	}
	case 0x0EF: {	/* NtQueryVolumeInformationFile(handle, iosb, info, length, class) */
		uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, length = ctx.r6.u32, cls = ctx.r7.u32;
		if (cls == 3 && length >= 0x18) {	/* FileFsSizeInformation */
			m.W64(info + 0, 0x100000);		/* total allocation units */
			m.W64(info + 8, 0x80000);		/* available */
			m.W32(info + 0x10, 1);			/* sectors per unit */
			m.W32(info + 0x14, 0x4000);		/* bytes per sector */
			IoStatus(iosb, STATUS_SUCCESS, 0x18);
			r = STATUS_SUCCESS;
		} else {
			g->Log(QAA_LOG_WARNING, "kernel: NtQueryVolumeInformationFile class %u not implemented", cls);
			IoStatus(iosb, STATUS_NOT_IMPLEMENTED, 0);
			r = STATUS_NOT_IMPLEMENTED;
		}
		break;
	}
	case 0x0E4:		/* NtQueryDirectoryFile(handle, event, apc, apc_context, iosb, info, length, pattern, restart) */
		r = QueryDirectoryFile(ctx);
		break;
	case 0x0DB:		/* NtFlushBuffersFile(handle, iosb) */
		IoStatus(ctx.r4.u32, STATUS_SUCCESS, 0);
		r = STATUS_SUCCESS;
		break;

	/* -- memory ----------------------------------------------------------------------------- */
	case 0x0CC: {	/* NtAllocateVirtualMemory(base*, size*, type, protect, debug) */
		uint32_t bp = ctx.r3.u32, sp = ctx.r4.u32, type = ctx.r5.u32;
		uint32_t a = m.U32(bp), size = m.U32(sp);
		r = g->arena.Allocate(&a, &size, type, (type & 0x100000) != 0);	/* MEM_TOP_DOWN */
		if (r == STATUS_SUCCESS) {
			m.W32(bp, a);
			m.W32(sp, size);
		} else {
			g->Log(QAA_LOG_WARNING, "kernel: NtAllocateVirtualMemory(%08X, %08X, %08X) failed %08X", a, size, type, (uint32_t)r);
		}
		break;
	}
	case 0x0DC: {	/* NtFreeVirtualMemory(base*, size*, type, debug) */
		uint32_t bp = ctx.r3.u32, sp = ctx.r4.u32;
		uint32_t a = m.U32(bp), size = m.U32(sp);
		r = g->arena.Free(&a, &size, ctx.r5.u32);
		if (r == STATUS_SUCCESS) {
			m.W32(bp, a);
			m.W32(sp, size);
		}
		break;
	}
	case 0x0EE: {	/* NtQueryVirtualMemory(address, MEMORY_BASIC_INFORMATION*, debug) */
		uint32_t info[7];
		uint32_t a = ctx.r3.u32;
		if (g->arena.Query(a, info)) {
			for (int i = 0; i < 7; i++)
				m.W32(ctx.r4.u32 + 4 * i, info[i]);
			r = STATUS_SUCCESS;
		} else if (m.IsCommitted(a, 1)) {
			/* outside the arena: images, stacks, kernel data */
			uint32_t b = a & ~0xFFFu, e = b;
			while (m.IsCommitted(e, 1))
				e += 0x1000;
			uint32_t v[7] = { b, b, 0x04, e - b, 0x1000, 0x04, a >= QAA_GUEST_IMAGE_BASE && a < QAA_GUEST_IMAGE_END ? 0x1000000u : 0x20000u };
			for (int i = 0; i < 7; i++)
				m.W32(ctx.r4.u32 + 4 * i, v[i]);
			r = STATUS_SUCCESS;
		} else {
			r = STATUS_INVALID_PARAMETER;
		}
		break;
	}
	case 0x11B: {	/* RtlCompareMemoryUlong(source, length, pattern) */
		uint32_t src = ctx.r3.u32, len = ctx.r4.u32 & ~3u, pat = ctx.r5.u32;
		uint32_t n = 0;
		while (n < len && m.U32(src + n) == pat)
			n += 4;
		r = n;
		break;
	}

	/* -- critical sections ---------------------------------------------------------------------- */
	case 0x12E:		/* RtlInitializeCriticalSection(cs) */
		CsInit(ctx.r3.u32);
		break;
	case 0x12F:		/* RtlInitializeCriticalSectionAndSpinCount(cs, spin) */
		CsInit(ctx.r3.u32);
		r = STATUS_SUCCESS;
		break;
	case 0x125:		/* RtlEnterCriticalSection(cs) */
		CsEnter(ctx.r3.u32, false);
		break;
	case 0x141:		/* RtlTryEnterCriticalSection(cs) */
		r = CsEnter(ctx.r3.u32, true);
		break;
	case 0x130:		/* RtlLeaveCriticalSection(cs) */
		CsLeave(ctx.r3.u32);
		break;

	/* -- strings -------------------------------------------------------------------------------- */
	case 0x12C: {	/* RtlInitAnsiString(dest, source) */
		uint32_t d = ctx.r3.u32, s = ctx.r4.u32;
		size_t n = s ? qaa_guest_strnlen((qaa_guest *)g, s, 0xFFFE) : 0;
		m.W16(d, (uint16_t)n);
		m.W16(d + 2, (uint16_t)(s ? n + 1 : 0));
		m.W32(d + 4, s);
		break;
	}
	case 0x12D: {	/* RtlInitUnicodeString(dest, source) */
		uint32_t d = ctx.r3.u32, s = ctx.r4.u32;
		uint32_t n = 0;
		if (s)
			while (n < 0x7FFE && m.U16(s + 2 * n))
				n++;
		m.W16(d, (uint16_t)(2 * n));
		m.W16(d + 2, (uint16_t)(s ? 2 * n + 2 : 0));
		m.W32(d + 4, s);
		break;
	}
	case 0x127: {	/* RtlFreeAnsiString(string) */
		uint32_t s = ctx.r3.u32;
		uint32_t buf = m.U32(s + 4);
		if (buf)
			g->pool.Free(buf);
		m.W32(s, 0);
		m.W32(s + 4, 0);
		break;
	}
	case 0x142: {	/* RtlUnicodeStringToAnsiString(dest, source, allocate) */
		uint32_t d = ctx.r3.u32, s = ctx.r4.u32;
		uint32_t chars = m.U16(s) / 2, src = m.U32(s + 4);
		uint32_t buf;
		if (ctx.r5.u32) {
			buf = g->pool.Alloc(chars + 1);
			if (!buf) {
				r = STATUS_NO_MEMORY;
				break;
			}
			m.W16(d + 2, (uint16_t)(chars + 1));
			m.W32(d + 4, buf);
		} else {
			buf = m.U32(d + 4);
			if (m.U16(d + 2) < chars + 1) {
				r = STATUS_BUFFER_TOO_SMALL;
				break;
			}
		}
		for (uint32_t i = 0; i < chars; i++) {
			uint16_t c = m.U16(src + 2 * i);
			m.W8(buf + i, c < 0x100 ? (uint8_t)c : '?');
		}
		m.W8(buf + chars, 0);
		m.W16(d, (uint16_t)chars);
		r = STATUS_SUCCESS;
		break;
	}
	case 0x133: {	/* RtlMultiByteToUnicodeN(unicode, max_bytes, bytes_written*, multibyte, mb_bytes) */
		uint32_t out = ctx.r3.u32, max = ctx.r4.u32 / 2, written = ctx.r5.u32, src = ctx.r6.u32, n = ctx.r7.u32;
		uint32_t k = std::min(max, n);
		for (uint32_t i = 0; i < k; i++)
			m.W16(out + 2 * i, m.U8(src + i));
		if (written)
			m.W32(written, 2 * k);
		r = STATUS_SUCCESS;
		break;
	}
	case 0x135: {	/* RtlNtStatusToDosError(status) */
		uint32_t s = ctx.r3.u32;
		switch (s) {
		case STATUS_SUCCESS: r = 0; break;
		case STATUS_TIMEOUT: r = 1460; break;
		case STATUS_NO_MORE_FILES: r = 18; break;
		case STATUS_NOT_IMPLEMENTED: r = 1; break;
		case STATUS_INVALID_HANDLE: r = 6; break;
		case STATUS_INVALID_PARAMETER: r = 87; break;
		case STATUS_NO_SUCH_FILE:
		case STATUS_OBJECT_NAME_NOT_FOUND: r = 2; break;
		case STATUS_OBJECT_PATH_NOT_FOUND: r = 3; break;
		case STATUS_END_OF_FILE: r = 38; break;
		case STATUS_NO_MEMORY: r = 8; break;
		case STATUS_ACCESS_DENIED: r = 5; break;
		case STATUS_BUFFER_TOO_SMALL: r = 122; break;
		case STATUS_OBJECT_NAME_COLLISION: r = 183; break;
		case STATUS_FILE_IS_A_DIRECTORY: r = 5; break;
		default: r = (s & 0xFFFF0000) == 0xC0070000 ? (s & 0xFFFF) : 317; break;
		}
		break;
	}

	/* -- process, time, XEX ------------------------------------------------------------------------ */
	case 0x066:		/* KeGetCurrentProcessType(): title process */
		r = 1;
		break;
	case 0x083:		/* KeQueryPerformanceFrequency() */
		r = (uint64_t)g->tsc_frequency;
		break;
	case 0x084:		/* KeQuerySystemTime(time*) */
		if (ctx.r3.u32)
			m.W64(ctx.r3.u32, FileTimeNow());
		break;
	case 0x12B: {	/* RtlImageXexHeaderField(xex_header, key) */
		uint32_t hdr = ctx.r3.u32, key = ctx.r4.u32;
		r = 0;
		if (hdr && m.IsCommitted(hdr, 0x18)) {
			uint32_t n = m.U32(hdr + 0x14);
			for (uint32_t i = 0; i < n && i < 64; i++) {
				uint32_t k = m.U32(hdr + 0x18 + 8 * i), v = m.U32(hdr + 0x1C + 8 * i);
				if (k == key) {
					r = (key & 0xFF) == 0 ? v : (key & 0xFF) == 1 ? hdr + 0x1C + 8 * i : hdr + v;
					break;
				}
			}
		}
		if (g->trace_kernel || !r)
			g->Log(QAA_LOG_DEBUG, "kernel: RtlImageXexHeaderField(%08X, %08X) -> %08X", hdr, key, (uint32_t)r);
		break;
	}

	/* -- errors ------------------------------------------------------------------------------------- */
	case 0x052:		/* KeBugCheck(code) */
		Fatal("KeBugCheck(%08X)", ctx.r3.u32);
	case 0x053:		/* KeBugCheckEx(code, p1, p2, p3, p4) */
		Fatal("KeBugCheckEx(%08X, %08X, %08X, %08X, %08X)", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
	case 0x136: {	/* RtlRaiseException(EXCEPTION_RECORD*) */
		uint32_t rec = ctx.r3.u32;
		uint32_t code = m.U32(rec);
		if (code == 0x406D1388) {	/* MSVC thread naming: no debugger to tell */
			break;
		}
		RaiseGuestException(ctx, rec);
		return;	/* registers are set for the resume (or the call continues) */
	}
	case 0x1A5:		/* __C_specific_handler: only the exception dispatcher calls it */
		Fatal("__C_specific_handler called directly");

	default:
		if (RunImportHook(g, library, ordinal, ctx))
			return;
		Fatal("call to unimplemented xboxkrnl ordinal 0x%03X", ordinal);
	}
	ctx.r3.u64 = r;
}

} // namespace qaa
