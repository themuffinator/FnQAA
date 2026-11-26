/*
 * Unit tests for the guest runtime, against the synthetic module
 * (tests/synthetic): loading, imports, calls, syscalls with stack
 * arguments, nested calls, kernel HLE, faults. Headless; no retail data.
 */
#include "qaa_guest.h"

#include <cstdio>
#include <cstring>
#include <string>

/* The synthetic module library: argv[1], or the path the build defines. */
#ifndef QAA_SYNTHETIC_MODULE
#define QAA_SYNTHETIC_MODULE nullptr
#endif

static int failures = 0, checks = 0;

#define CHECK(cond)                                                              \
	do {                                                                         \
		checks++;                                                                \
		if (!(cond)) {                                                           \
			failures++;                                                          \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                        \
	} while (0)

/* ---- host: one in-memory file --------------------------------------------- */

static const char file_data[] = "synthetic file contents";

static void HostLog(void *, int level, const char *msg) {
	if (level >= QAA_LOG_WARNING)
		fprintf(stderr, "  [guest log %d] %s\n", level, msg);
}

static int HostDirEntry(void *, const char *path, uint32_t index, char *name, size_t size, qaa_file_info *info) {
	static const char *entries[] = { "a.txt", "b.cfg", "cc.txt" };
	if (strcmp(path, "game:\\d") || index >= 3)
		return -1;
	snprintf(name, size, "%s", entries[index]);
	info->size = 10 * (index + 1);
	info->is_directory = 0;
	return 0;
}

static int HostStat(void *, const char *path, qaa_file_info *info) {
	if (!strcmp(path, "game:\\d")) {
		info->size = 0;
		info->is_directory = 1;
		return 0;
	}
	if (strcmp(path, "game:\\test.txt"))
		return -1;
	info->size = sizeof(file_data) - 1;
	info->is_directory = 0;
	return 0;
}

static int64_t HostOpen(void *, const char *path, int access, int) {
	return strcmp(path, "game:\\test.txt") || (access & QAA_FILE_WRITE) ? -1 : 7;
}

static int64_t HostRead(void *, int64_t h, int64_t off, void *buf, uint32_t len) {
	if (h != 7)
		return -1;
	int64_t size = sizeof(file_data) - 1;
	if (off >= size)
		return 0;
	int64_t n = size - off < len ? size - off : len;
	memcpy(buf, file_data + off, (size_t)n);
	return n;
}

static int64_t HostSize(void *, int64_t) { return sizeof(file_data) - 1; }
static int64_t resized = -1;
static int HostSetSize(void *, int64_t h, int64_t size) {
	if (h != 7)
		return -1;
	resized = size;
	return 0;
}
static int closes = 0;
static void HostClose(void *, int64_t) { closes++; }

/* ---- syscalls --------------------------------------------------------------- */

struct SyscallLog {
	qaa_syscall last = {};
	int calls = 0;
	std::string string_arg;
};

static int64_t Syscall(void *user, qaa_module *m, const qaa_syscall *c) {
	auto *s = (SyscallLog *)user;
	qaa_guest *g = qaa_module_guest(m);
	s->last = *c;
	s->calls++;
	switch (c->trap) {
	case 100: {
		size_t n = qaa_guest_strnlen(g, (uint32_t)c->args[0], 64);
		s->string_arg.assign((const char *)qaa_guest_translate(g, (uint32_t)c->args[0]), n);
		return 1000;
	}
	case 200: {	/* nested: vmMain(2, 40, 2) adds */
		int32_t args[2] = { 40, 2 };
		int64_t r = 0;
		if (qaa_module_vm_main(m, 2, args, 2, &r) != QAA_OK)
			return -1;
		return r + 1000;
	}
	case 300:
		qaa_guest_abort(g, "aborted by the test");
	default:
		return -1;
	}
}

int main(int argc, char **argv) {
	const char *module_path = argc > 1 ? argv[1] : QAA_SYNTHETIC_MODULE;
	if (!module_path) {
		fprintf(stderr, "usage: qaa_guest_tests <synthetic module library>\n");
		return 2;
	}
	qaa_host host = {};
	host.log = HostLog;
	host.file_stat = HostStat;
	host.file_open = HostOpen;
	host.file_read = HostRead;
	host.file_size = HostSize;
	host.file_close = HostClose;
	host.dir_entry = HostDirEntry;
	host.file_set_size = HostSetSize;
	char err[512] = "";
	qaa_guest *g = qaa_guest_create(&host, err, sizeof(err));
	CHECK(g != nullptr);
	if (!g) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}

	/* guest memory helpers */
	uint32_t a = qaa_guest_alloc(g, 64);
	CHECK(a >= QAA_GUEST_KERNEL_BASE && a < QAA_GUEST_KERNEL_END);
	CHECK(qaa_guest_is_mapped(g, a, 64));
	CHECK(!qaa_guest_is_mapped(g, 0, 4));
	qaa_guest_store32(g, a, 0x11223344);
	CHECK(((uint8_t *)qaa_guest_translate(g, a))[0] == 0x11);
	CHECK(qaa_guest_load32(g, a) == 0x11223344);
	CHECK(qaa_guest_load16(g, a + 2) == 0x3344);
	qaa_guest_store64(g, a + 8, 0x0102030405060708ull);
	CHECK(qaa_guest_load64(g, a + 8) == 0x0102030405060708ull);
	CHECK(qaa_guest_load32(g, a + 8) == 0x01020304);
	qaa_guest_storef(g, a + 16, 1.5f);
	CHECK(qaa_guest_load32(g, a + 16) == 0x3FC00000);
	CHECK(qaa_guest_loadf(g, a + 16) == 1.5f);
	qaa_guest_stored(g, a + 24, -2.0);
	CHECK(qaa_guest_loadd(g, a + 24) == -2.0);
	qaa_guest_write(g, a + 32, "abc", 4);
	CHECK(qaa_guest_strnlen(g, a + 32, 16) == 3);
	uint32_t b = qaa_guest_alloc(g, 64);
	CHECK(b != a);
	qaa_guest_free(g, a);
	uint32_t c = qaa_guest_alloc(g, 32);
	CHECK(c == a);	/* first fit reuses the freed block */

	/* the executable's XEX header (made up): 0x20401 = 0x00300000 */
	static const uint8_t exe_header[0x20] = {
		'X', 'E', 'X', '2', 0, 0, 0, 0, 0, 0, 0, 0x20, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 1, 0, 0x02, 0x04, 0x01, 0x00, 0x30, 0x00, 0x00,
	};
	CHECK(qaa_guest_set_executable_header(g, "nope", 4) == QAA_ERROR);
	CHECK(qaa_guest_set_executable_header(g, exe_header, sizeof(exe_header)) == QAA_OK);

	/* load: maps the image, binds imports, runs the entry point */
	qaa_module *m = qaa_module_load(g, module_path, err, sizeof(err));
	CHECK(m != nullptr);
	if (!m) {
		fprintf(stderr, "load: %s\n", err);
		return 1;
	}
	CHECK(qaa_module_base(m) == 0x82000000);
	CHECK(qaa_module_export(m, QAA_EXPORT_VMMAIN) == 0x82010200);
	CHECK(qaa_module_export(m, 3) == 0);
	CHECK(qaa_guest_load32(g, 0x82002008) == 6);	/* XboxHardwareInfo.NumberOfProcessors */
	CHECK(qaa_guest_load32(g, 0x82000404) == 0x82010F00);	/* function slot -> thunk */
	CHECK(qaa_guest_load32(g, 0x82002004) < 64);	/* KeTlsAlloc in the entry point */
	CHECK(qaa_guest_is_mapped(g, 0x8201FFFC, 4));	/* BSS mapped up to the image size */

	SyscallLog log;
	CHECK(qaa_module_dll_entry(m, Syscall, &log) == QAA_OK);
	uint32_t sysaddr = qaa_guest_load32(g, 0x82002000);
	CHECK(sysaddr >= QAA_GUEST_HOSTCALL_BASE && sysaddr < QAA_GUEST_HOSTCALL_END);

	int64_t r = 0;
	/* 0: syscall arguments in registers, on the stack and in FPRs */
	CHECK(qaa_module_vm_main(m, 0, nullptr, 0, &r) == QAA_OK);
	CHECK(r == 1001);
	CHECK(log.last.trap == 100);
	CHECK(log.string_arg == "hello");
	CHECK(log.last.args[1] == 2 && log.last.args[5] == -6 && log.last.args[6] == 7);
	CHECK(log.last.args[7] == 8 && log.last.args[8] == 9);
	CHECK(log.last.fargs[0] == 1.5);
	CHECK(log.last.return_address == 0x82010280);

	/* 1: virtual memory */
	CHECK(qaa_module_vm_main(m, 1, nullptr, 0, &r) == QAA_OK);
	CHECK(r >= QAA_GUEST_HEAP_BASE && r < QAA_GUEST_HEAP_END && (r & 0xFFFF) == 0);

	/* 2: indirect call */
	int32_t args[2] = { 30, 12 };
	CHECK(qaa_module_vm_main(m, 2, args, 2, &r) == QAA_OK);
	CHECK(r == 42);

	/* 3: guest memory fault is caught and reported */
	CHECK(qaa_module_vm_main(m, 3, nullptr, 0, &r) == QAA_ERROR);
	CHECK(strstr(qaa_guest_last_error(g), "unmapped guest address 00000010") != nullptr);

	/* 4: trap */
	CHECK(qaa_module_vm_main(m, 4, nullptr, 0, &r) == QAA_ERROR);
	CHECK(strstr(qaa_guest_last_error(g), "82010244") != nullptr);

	/* the guest still works after a fault */
	CHECK(qaa_module_vm_main(m, 2, args, 2, &r) == QAA_OK && r == 42);

	/* 5: critical section recursion */
	CHECK(qaa_module_vm_main(m, 5, nullptr, 0, &r) == QAA_OK);
	CHECK(r == 20);

	/* 6: nested call from inside a syscall */
	CHECK(qaa_module_vm_main(m, 6, nullptr, 0, &r) == QAA_OK);
	CHECK(r == 1042);

	/* 7: guest thread and event */
	CHECK(qaa_module_vm_main(m, 7, nullptr, 0, &r) == QAA_OK);
	CHECK(r == 0x1255);

	/* 8: TLS */
	CHECK(qaa_module_vm_main(m, 8, nullptr, 0, &r) == QAA_OK);
	CHECK(r == 0xABCD);

	/* 9: file read through the host */
	CHECK(qaa_module_vm_main(m, 9, nullptr, 0, &r) == QAA_OK);
	CHECK(r == (int64_t)strlen(file_data));
	CHECK(memcmp(qaa_guest_translate(g, 0x82002040), file_data, strlen(file_data)) == 0);
	CHECK(closes == 1);

	/* 10: indirect call to nothing */
	CHECK(qaa_module_vm_main(m, 10, nullptr, 0, &r) == QAA_ERROR);
	CHECK(strstr(qaa_guest_last_error(g), "82012000") != nullptr);

	/* 12, 13: RtlImageXexHeaderField on the module's and the executable's header */
	CHECK(qaa_module_vm_main(m, 12, nullptr, 0, &r) == QAA_OK);
	CHECK(r == 0x00200000);
	CHECK(qaa_module_vm_main(m, 13, nullptr, 0, &r) == QAA_OK);
	CHECK(r == 0x00300000);

	/* 14: directory listing with a wildcard: a.txt and cc.txt */
	CHECK(qaa_module_vm_main(m, 14, nullptr, 0, &r) == QAA_OK);
	CHECK(r == 206);

	/* 16: NtSetInformationFile end of file goes to the host */
	CHECK(qaa_module_vm_main(m, 16, nullptr, 0, &r) == QAA_OK);
	CHECK(r == 0 && resized == 7);

	/* 15: guest SEH. No raise: 5 + 100 + 1000. */
	int32_t mode = 0;
	CHECK(qaa_module_vm_main(m, 15, &mode, 1, &r) == QAA_OK);
	CHECK(r == 1105);
	CHECK(qaa_guest_load32(g, 0x82002304) == 0);
	/* NO_MEMORY: the outer filter accepts; mid's __finally runs (abnormal,
	 * with mid's caller SP); r31 comes back from mid's save; the handler runs */
	mode = 1;
	CHECK(qaa_module_vm_main(m, 15, &mode, 1, &r) == QAA_OK);
	CHECK(r == 7000);
	CHECK(qaa_guest_load32(g, 0x82002304) == 1);
	CHECK(qaa_guest_load32(g, 0x82002308) == qaa_guest_load32(g, 0x82002310));
	CHECK(qaa_guest_load32(g, 0x8200230C) == 1);
	/* the filter rejects E0001234: unhandled, the call fails */
	mode = 2;
	CHECK(qaa_module_vm_main(m, 15, &mode, 1, &r) == QAA_ERROR);
	CHECK(strstr(qaa_guest_last_error(g), "unhandled guest exception E0001234") != nullptr);
	/* and dispatch still works afterwards */
	mode = 1;
	CHECK(qaa_module_vm_main(m, 15, &mode, 1, &r) == QAA_OK && r == 7000);

	/* 11: the host aborts from a syscall */
	CHECK(qaa_module_vm_main(m, 11, nullptr, 0, &r) == QAA_ERROR);
	CHECK(strstr(qaa_guest_last_error(g), "aborted by the test") != nullptr);

	CHECK(qaa_module_vm_main(m, 2, args, 2, &r) == QAA_OK && r == 42);

	qaa_module_unload(m);
	CHECK(!qaa_guest_is_mapped(g, 0x82000000, 4));
	qaa_guest_destroy(g);

	printf("qaa_guest_tests: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
