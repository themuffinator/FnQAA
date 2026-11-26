/*
 * openQAA guest runtime: public C API.
 *
 * The runtime hosts Quake Arena Arcade's statically recompiled Xbox 360
 * game modules. It owns a 4 GiB guest address space laid out like the
 * Xbox's (modules at their image bases, big-endian data), implements the
 * xboxkrnl functions the modules import, and lets the host call the
 * modules' dllEntry and vmMain.
 *
 * It knows nothing about the engine: files go through qaa_host callbacks,
 * and the module's engine calls (traps) go to the syscall callback given
 * to qaa_module_dll_entry with their guest-address arguments untouched.
 */
#ifndef QAA_GUEST_H
#define QAA_GUEST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qaa_guest qaa_guest;
typedef struct qaa_module qaa_module;

enum {
	QAA_OK = 0,
	QAA_ERROR = -1,		/* see qaa_guest_last_error() */
};

enum {
	QAA_LOG_DEBUG,
	QAA_LOG_INFO,
	QAA_LOG_WARNING,
	QAA_LOG_ERROR,
};

/* File access the guest asks for (NtCreateFile/NtOpenFile). */
enum {
	QAA_FILE_READ = 1,
	QAA_FILE_WRITE = 2,
};

/* Create dispositions, as NT's FILE_SUPERSEDE .. FILE_OVERWRITE_IF. */
enum {
	QAA_FILE_SUPERSEDE = 0,
	QAA_FILE_OPEN = 1,
	QAA_FILE_CREATE = 2,
	QAA_FILE_OPEN_IF = 3,
	QAA_FILE_OVERWRITE = 4,
	QAA_FILE_OVERWRITE_IF = 5,
};

typedef struct qaa_file_info {
	int64_t size;
	int is_directory;
} qaa_file_info;

/*
 * Host services. Every member may be NULL: a NULL log writes to stderr,
 * NULL file functions make every file appear absent. Paths are the
 * guest's, unchanged (for example "game:\\baseq3\\autoexec.cfg" or
 * "\\Device\\Harddisk0\\..."); mapping them is the host's job.
 */
typedef struct qaa_host {
	void *user;
	void (*log)(void *user, int level, const char *message);
	/* Returns a handle >= 0, or < 0 if the file cannot be opened. */
	int64_t (*file_open)(void *user, const char *path, int access, int disposition);
	/* Returns 0 and fills info if the path exists, < 0 if not. */
	int (*file_stat)(void *user, const char *path, qaa_file_info *info);
	/* Return bytes transferred, or < 0 on error. */
	int64_t (*file_read)(void *user, int64_t handle, int64_t offset, void *buffer, uint32_t length);
	int64_t (*file_write)(void *user, int64_t handle, int64_t offset, const void *buffer, uint32_t length);
	int64_t (*file_size)(void *user, int64_t handle);
	void (*file_close)(void *user, int64_t handle);
	/*
	 * Directory listing (NtQueryDirectoryFile): the index-th entry of the
	 * directory at path, in any stable order. Fill name (NUL-terminated,
	 * no path) and info; return 0, or < 0 when index is past the end.
	 * The runtime applies the guest's wildcard pattern itself.
	 */
	int (*dir_entry)(void *user, const char *path, uint32_t index, char *name, size_t name_size, qaa_file_info *info);
	/* Truncate or extend an open file (NtSetInformationFile end of file); 0 or < 0. */
	int (*file_set_size)(void *user, int64_t handle, int64_t size);
} qaa_host;

/*
 * An engine call made by a module through the pointer it got from
 * dllEntry: the trap number (r3) and its arguments as the guest passed
 * them, 32-bit values (integers, guest addresses, or floats passed as
 * their bits). args[0..6] are r4..r10, args[7..] the caller's stack
 * argument slots. fargs are f1..f13 at the time of the call.
 */
#define QAA_SYSCALL_MAX_ARGS 16

typedef struct qaa_syscall {
	int32_t trap;
	int32_t args[QAA_SYSCALL_MAX_ARGS];
	double fargs[13];
	uint32_t return_address;	/* guest LR: the call site, for diagnostics */
} qaa_syscall;

/* Returns the value the guest sees in r3. */
typedef int64_t (*qaa_syscall_fn)(void *user, qaa_module *module, const qaa_syscall *call);

/* ---- lifetime ----------------------------------------------------------- */

qaa_guest *qaa_guest_create(const qaa_host *host, char *error, size_t error_size);
void qaa_guest_destroy(qaa_guest *guest);

/*
 * The XEX header of the title executable (default.xex: the file's first
 * bytes up to its PE data offset). Modules read its optional headers
 * through XexExecutableModuleHandle and RtlImageXexHeaderField (the CRT
 * asks for the default heap size). Call before qaa_module_load; without
 * it the header has no optional headers and the CRT uses its defaults.
 */
int qaa_guest_set_executable_header(qaa_guest *guest, const void *header, size_t size);

/* What the last failed call into the guest hit (fault, trap, bug check). */
const char *qaa_guest_last_error(const qaa_guest *guest);

/*
 * Load a recompiled module library: map its image at its base address,
 * bind its kernel imports and run its entry point (DLL_PROCESS_ATTACH).
 */
qaa_module *qaa_module_load(qaa_guest *guest, const char *library_path, char *error, size_t error_size);
void qaa_module_unload(qaa_module *module);

const char *qaa_module_name(const qaa_module *module);
uint32_t qaa_module_base(const qaa_module *module);
uint32_t qaa_module_size(const qaa_module *module);
qaa_guest *qaa_module_guest(const qaa_module *module);
/* Guest address of an export, or 0. */
uint32_t qaa_module_export(const qaa_module *module, uint32_t ordinal);

/*
 * QAA's modules export vmMain as ordinal 1 and dllEntry as ordinal 2.
 * qaa_module_dll_entry passes the module a guest address that calls fn.
 */
enum { QAA_EXPORT_VMMAIN = 1, QAA_EXPORT_DLLENTRY = 2 };

int qaa_module_dll_entry(qaa_module *module, qaa_syscall_fn fn, void *user);

/*
 * A guest function address that calls fn: for function tables the host
 * hands a module. The callback gets the same qaa_syscall record; for a
 * host function its "trap" field is simply r3, the first argument.
 * Returns 0 when the host-call range is exhausted.
 */
uint32_t qaa_module_host_function(qaa_module *module, qaa_syscall_fn fn, void *user);
int qaa_module_vm_main(qaa_module *module, int32_t command, const int32_t *args, int nargs, int64_t *result);

/*
 * From inside a syscall callback: abandon the current call into the guest
 * (as Com_Error does for a native module). The interrupted qaa_module_*
 * call returns QAA_ERROR with message as the last error. Does not return;
 * the callback's own frames are unwound without running destructors.
 */
#if defined(__cplusplus)
[[noreturn]]
#endif
void qaa_guest_abort(qaa_guest *guest, const char *message);

/* Call any guest function with up to 16 integer arguments. */
int qaa_guest_call(qaa_guest *guest, uint32_t function, const int64_t *args, int nargs, int64_t *result);

/* ---- hooks: guest functions the host implements --------------------------- */

/*
 * A module recompiled from part of an image (the front end inside
 * default.xex, docs/UI.md) calls functions that were not recompiled: the
 * engine code openQAA replaces. Each such call, direct or indirect, comes
 * to the host's hook for that guest address with the guest's registers.
 * The handler reads the arguments, does the work and writes the results.
 * It may call into the guest again (qaa_guest_call*).
 */
typedef struct qaa_hook_call {
	uint32_t address;			/* the guest function called */
	uint32_t return_address;	/* guest LR: the call site */
	uint32_t sp;				/* guest r1: stack arguments are the low words at sp + 0x54 + 8k */
	uint64_t r[8];				/* r3..r10 on entry; r[0] (r3) is the result */
	double f[13];				/* f1..f13 on entry; f[0] (f1) is the float result */
} qaa_hook_call;

typedef void (*qaa_hook_fn)(void *user, qaa_hook_call *call);

/* Handle calls to address with fn (NULL removes the hook). */
int qaa_guest_set_hook(qaa_guest *guest, uint32_t address, qaa_hook_fn fn, void *user);
/* For calls to a non-recompiled address that has no hook; without one such a
 * call is a fatal guest error. */
void qaa_guest_set_default_hook(qaa_guest *guest, qaa_hook_fn fn, void *user);

/*
 * Kernel imports the runtime does not implement (the front end imports far
 * more of xboxkrnl and XAM than the game modules): library is
 * "xboxkrnl.exe" or "xam.xex". call->address is library << 16 | ordinal (0 xboxkrnl, 1 XAM). The default
 * import hook takes every unimplemented import of every library.
 */
int qaa_guest_set_import_hook(qaa_guest *guest, const char *library, uint32_t ordinal, qaa_hook_fn fn, void *user);
void qaa_guest_set_default_import_hook(qaa_guest *guest, qaa_hook_fn fn, void *user);

/* Call a guest function with up to 8 integer and 13 float arguments. */
int qaa_guest_call_regs(qaa_guest *guest, uint32_t function, const uint64_t *r, int nr, const double *f, int nf,
	uint64_t *r3, double *f1);

/*
 * Lockstep with a guest worker thread: wait until the guest thread with
 * the given thread handle is blocked waiting on the object behind
 * event_handle (or has exited). Returns 0, or -1 on timeout (timeout_ms < 0
 * waits forever) or for a bad handle. The front end's update thread runs
 * one iteration per frame event; the engine signals it and waits here, so
 * the engine and the thread never run at the same time.
 */
int qaa_guest_wait_thread_blocked(qaa_guest *guest, uint32_t thread_handle, uint32_t event_handle, int timeout_ms);
/* One step of such a thread: it must be parked on event_handle; signal the
 * event and wait until the thread enters its next wait on it (or exits).
 * Returns 0, or -1 on timeout or a bad handle. */
int qaa_guest_step_thread(qaa_guest *guest, uint32_t thread_handle, uint32_t event_handle, int timeout_ms);
/* Signal an event handle the guest created (as NtSetEvent). */
int qaa_guest_set_event(qaa_guest *guest, uint32_t event_handle);

/* ---- guest memory ------------------------------------------------------- */

/* Host address of guest address 0; guest address a is at base + a. */
uint8_t *qaa_guest_base(const qaa_guest *guest);
/* Host pointer for a guest address (no checks). */
void *qaa_guest_translate(const qaa_guest *guest, uint32_t address);
/* Nonzero if [address, address + size) is committed guest memory. */
int qaa_guest_is_mapped(const qaa_guest *guest, uint32_t address, uint32_t size);

/* Big-endian loads and stores. */
uint8_t qaa_guest_load8(const qaa_guest *guest, uint32_t address);
uint16_t qaa_guest_load16(const qaa_guest *guest, uint32_t address);
uint32_t qaa_guest_load32(const qaa_guest *guest, uint32_t address);
uint64_t qaa_guest_load64(const qaa_guest *guest, uint32_t address);
float qaa_guest_loadf(const qaa_guest *guest, uint32_t address);
double qaa_guest_loadd(const qaa_guest *guest, uint32_t address);
void qaa_guest_store8(qaa_guest *guest, uint32_t address, uint8_t value);
void qaa_guest_store16(qaa_guest *guest, uint32_t address, uint16_t value);
void qaa_guest_store32(qaa_guest *guest, uint32_t address, uint32_t value);
void qaa_guest_store64(qaa_guest *guest, uint32_t address, uint64_t value);
void qaa_guest_storef(qaa_guest *guest, uint32_t address, float value);
void qaa_guest_stored(qaa_guest *guest, uint32_t address, double value);

/* Byte copies (no swapping). */
void qaa_guest_read(const qaa_guest *guest, uint32_t address, void *dst, size_t size);
void qaa_guest_write(qaa_guest *guest, uint32_t address, const void *src, size_t size);
/* Length of the NUL-terminated string at address, scanning at most max bytes. */
size_t qaa_guest_strnlen(const qaa_guest *guest, uint32_t address, size_t max);

/* Guest heap for the host's own use (strings and structures it passes in). */
uint32_t qaa_guest_alloc(qaa_guest *guest, uint32_t size);
void qaa_guest_free(qaa_guest *guest, uint32_t address);

/* ---- the guest memory map (see docs/RECOMP.md) --------------------------- */

#define QAA_GUEST_NULL_GUARD_END	0x00010000u
#define QAA_GUEST_HEAP_BASE			0x40000000u	/* NtAllocateVirtualMemory */
#define QAA_GUEST_HEAP_END			0x70000000u
#define QAA_GUEST_STACK_BASE		0x70000000u	/* thread stacks */
#define QAA_GUEST_STACK_END			0x7E000000u
#define QAA_GUEST_KERNEL_BASE		0x7E000000u	/* kernel objects, PCRs, HLE data */
#define QAA_GUEST_KERNEL_END		0x80000000u
#define QAA_GUEST_IMAGE_BASE		0x80000000u	/* module images at their bases */
#define QAA_GUEST_IMAGE_END			0x90000000u
#define QAA_GUEST_HOSTCALL_BASE		0x9F000000u	/* addresses that call into the host */
#define QAA_GUEST_HOSTCALL_END		0x9F100000u

#ifdef __cplusplus
}
#endif

#endif
