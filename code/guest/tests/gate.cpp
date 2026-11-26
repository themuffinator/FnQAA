/*
 * M3 gate: load a recompiled QAA module, call dllEntry with a stub engine
 * that logs every trap, then vmMain(INIT), and report the traps called
 * before it returns or fails. Headless: no window, audio or input.
 *
 *   qaa_gate [options] <module library> [init args...]
 *     --server LIB    first run this qagame library's GAME_INIT in the same
 *                     guest, then serve the configstrings it set to the
 *                     module's gamestate trap (for cgame)
 *     --command N     vmMain command (default 0: GAME_INIT / CG_INIT)
 *     --stubs KIND    game, cgame or none (default: from the file name)
 *     --error-trap N  trap that ends the call as an engine error (default 1,
 *                     G_ERROR / CG_ERROR; -1 for none)
 *     --max-traps N   stop after N traps (default 100000)
 *     --print N       print the first N traps in full (default 400)
 *     --timeout S     give up after S seconds (default 60)
 *     --entities S    entity string for the game's entity-token trap
 *                     (default one worldspawn: synthetic, not from a map)
 *     --files DIR     serve guest file paths ("game:\x" -> DIR/x) read-only
 *     --json FILE     write the trap sequence as JSON
 *
 * Stubs: every trap returns 0, except as listed in the profiles below.
 * Trap numbers there are either Quake III's legacy numbers (FnQL's
 * g_public.h / cg_public.h), which QAA follows for the low traps, or
 * observed in the gate runs (docs/RECOMP.md). QAA's real tables are M4's
 * work. Buffers are only written when the argument points at mapped guest
 * memory and the size argument is plausible.
 */
#include "qaa_guest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* ---- stub profiles ------------------------------------------------------------------- */

struct BufferStub {
	int trap, buffer_arg, size_arg;	/* indices into qaa_syscall::args (args[0] = first argument) */
};

struct Profile {
	const char *name;
	std::vector<BufferStub> buffers;	/* traps that fill a string buffer: give "" */
	int millis = -1;					/* MILLISECONDS: real time */
	int fopen = -1;						/* FS_FOPEN_FILE(path, handle *, mode): *handle = 0, -1 */
	int set_configstring = -1;			/* SET_CONFIGSTRING(num, string): recorded */
	int entity_token = -1;				/* GET_ENTITY_TOKEN(buffer, size): from --entities */
	int gamestate = -1;					/* GETGAMESTATE(gameState_t *): recorded configstrings */
	int import_table_command = -1;		/* vmMain(N, table) before INIT */
};

/* qagame. Q3 legacy: 2 MILLISECONDS, 7 CVAR_VARIABLE_STRING_BUFFER, 9 ARGV,
 * 10 FS_FOPEN_FILE, 18 SET_CONFIGSTRING, 19 GET_CONFIGSTRING,
 * 20 GET_USERINFO, 22 GET_SERVERINFO. Observed: 38 takes (buffer, size)
 * right before "SpawnEntities: no entities", so it is GET_ENTITY_TOKEN (Q3
 * 37); vmMain command 9 stores its argument as an engine function table. */
static const Profile game_profile = {
	"game", { { 7, 1, 2 }, { 9, 1, 2 }, { 19, 1, 2 }, { 20, 1, 2 }, { 22, 0, 1 } },
	2, 10, 18, 38, -1, 9,
};

/* cgame. Q3 legacy: 2 MILLISECONDS, 6 CVAR_VARIABLESTRINGBUFFER (observed
 * with "sv_running", buffer, 1024), 10 FS_FOPENFILE. Observed: 53 fills a
 * gameState_t whose stringData starts at +0x1000 (1024 offsets); vmMain
 * command 12 stores its argument as an engine function table. */
static const Profile cgame_profile = {
	"cgame", { { 6, 1, 2 } },
	2, 10, -1, -1, 53, 12,
};

static const Profile no_profile = { "none", {}, -1, -1, -1, -1, -1, -1 };

/* ---- state -------------------------------------------------------------------------------- */

enum { IMPORT_BASE = 100000 };	/* calls through the function table: IMPORT_BASE + byte offset */

struct Call {
	int32_t trap;
	int32_t args[QAA_SYSCALL_MAX_ARGS];
	uint32_t lr;
};

struct Run {
	std::string module;
	const Profile *profile = &no_profile;
	std::vector<Call> calls;
	std::map<int32_t, int> counts;
	std::map<int32_t, uint32_t> first_lr;
	std::string result;
	bool ok = false;
};

struct Gate {
	qaa_guest *guest = nullptr;
	std::mutex lock;
	std::vector<Run> runs;
	int error_trap = 1;
	int max_traps = 100000;
	int print = 400;
	std::string files;
	std::vector<FILE *> open_files;
	std::vector<std::string> file_log;
	std::string entities = "{ \"classname\" \"worldspawn\" }";
	size_t entity_pos = 0;
	std::map<int, std::string> configstrings;
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

static Gate gate;

static Run &Current() { return gate.runs.back(); }

/* ---- helpers --------------------------------------------------------------------------------- */

/* A printable NUL-terminated guest string at address, or empty. */
static std::string GuestString(qaa_guest *g, uint32_t a, size_t max = 96) {
	if (a < 0x10000 || a >= 0xA0000000u || !qaa_guest_is_mapped(g, a, 1))
		return {};
	size_t n = qaa_guest_strnlen(g, a, max);
	if (n == 0 || !qaa_guest_is_mapped(g, a, (uint32_t)n + 1))
		return {};
	std::string s((const char *)qaa_guest_translate(g, a), n);
	for (char c : s)
		if (((unsigned char)c < 0x20 && c != '\n' && c != '\t' && c != '\r') || (unsigned char)c >= 0x7F)
			return {};
	return s;
}

static std::string Escape(const std::string &s) {
	std::string o;
	for (char c : s) {
		if (c == '\n') o += "\\n";
		else if (c == '\r') o += "\\r";
		else if (c == '\t') o += "\\t";
		else if (c == '"' || c == '\\') { o += '\\'; o += c; }
		else o += c;
	}
	return o;
}

static std::string TrapName(int32_t t) {
	char b[32];
	if (t >= IMPORT_BASE)
		snprintf(b, sizeof(b), "imp+0x%02X", t - IMPORT_BASE);
	else
		snprintf(b, sizeof(b), "%d", t);
	return b;
}

static std::string Describe(qaa_guest *g, const Call &c, int nshow) {
	std::string out;
	char buf[64];
	for (int i = 0; i < nshow; i++) {
		uint32_t v = (uint32_t)c.args[i];
		std::string s = GuestString(g, v);
		if (!s.empty()) {
			snprintf(buf, sizeof(buf), " %08X", v);
			out += buf;
			out += "=\"" + Escape(s) + "\"";
		} else if (c.args[i] > -100000 && c.args[i] < 100000) {
			snprintf(buf, sizeof(buf), " %d", c.args[i]);
			out += buf;
		} else {
			snprintf(buf, sizeof(buf), " %08X", v);
			out += buf;
		}
	}
	return out;
}

/* COM_Parse-style: quoted strings and whitespace-separated words. */
static std::string NextEntityToken() {
	const std::string &s = gate.entities;
	size_t &p = gate.entity_pos;
	while (p < s.size() && (unsigned char)s[p] <= ' ')
		p++;
	if (p >= s.size())
		return {};
	std::string tok;
	if (s[p] == '"') {
		for (p++; p < s.size() && s[p] != '"'; p++)
			tok += s[p];
		p++;
	} else {
		for (; p < s.size() && (unsigned char)s[p] > ' '; p++)
			tok += s[p];
	}
	return tok;
}

static size_t Record(const Call &c) {
	std::lock_guard<std::mutex> l(gate.lock);
	Run &r = Current();
	r.calls.push_back(c);
	if (!r.counts[c.trap]++)
		r.first_lr[c.trap] = c.lr;
	return r.calls.size();
}

/* ---- the stub engine ------------------------------------------------------------------------- */

static int64_t ImportCall(void *user, qaa_module *m, const qaa_syscall *sc) {
	qaa_guest *g = qaa_module_guest(m);
	Call c;
	c.trap = IMPORT_BASE + 4 * (int32_t)(intptr_t)user;
	c.args[0] = sc->trap;	/* a host function's first argument (r3) */
	memcpy(c.args + 1, sc->args, sizeof(c.args) - sizeof(c.args[0]));
	c.lr = sc->return_address;
	size_t n = Record(c);
	if ((int)n <= gate.print)
		printf("  import +0x%02X  lr %08X  args%s\n", c.trap - IMPORT_BASE, c.lr, Describe(g, c, 8).c_str());
	if ((int)n >= gate.max_traps)
		qaa_guest_abort(g, "trap limit reached");
	return 0;
}

static int64_t Syscall(void *, qaa_module *m, const qaa_syscall *sc) {
	qaa_guest *g = qaa_module_guest(m);
	const Profile &p = *Current().profile;
	Call c;
	c.trap = sc->trap;
	memcpy(c.args, sc->args, sizeof(c.args));
	c.lr = sc->return_address;
	size_t n = Record(c);
	if ((int)n <= gate.print)
		printf("  trap %4d  lr %08X  args%s\n", c.trap, c.lr, Describe(g, c, 8).c_str());
	if (c.trap == gate.error_trap) {
		static char text[1200];
		std::string msg = GuestString(g, (uint32_t)c.args[0], 1024);
		snprintf(text, sizeof(text), "engine error trap %d: %s", c.trap, msg.empty() ? "(no message)" : msg.c_str());
		qaa_guest_abort(g, text);
	}
	if ((int)n >= gate.max_traps)
		qaa_guest_abort(g, "trap limit reached");

	for (const BufferStub &st : p.buffers) {
		if (c.trap != st.trap)
			continue;
		uint32_t buf = (uint32_t)c.args[st.buffer_arg];
		int32_t size = c.args[st.size_arg];
		if (size > 0 && size <= (1 << 20) && qaa_guest_is_mapped(g, buf, 1))
			qaa_guest_store8(g, buf, 0);
	}
	if (c.trap == p.millis)
		return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - gate.start).count();
	if (c.trap == p.fopen) {
		uint32_t handle = (uint32_t)c.args[1];
		if (handle && qaa_guest_is_mapped(g, handle, 4))
			qaa_guest_store32(g, handle, 0);
		return -1;
	}
	if (c.trap == p.set_configstring) {
		std::string s = GuestString(g, (uint32_t)c.args[1], 8192);
		std::lock_guard<std::mutex> l(gate.lock);
		gate.configstrings[c.args[0]] = s;
		return 0;
	}
	if (c.trap == p.entity_token) {
		uint32_t buf = (uint32_t)c.args[0];
		int32_t size = c.args[1];
		if (size <= 0 || !qaa_guest_is_mapped(g, buf, (uint32_t)size))
			return 0;
		std::string tok = NextEntityToken();
		size_t k = std::min(tok.size(), (size_t)size - 1);
		qaa_guest_write(g, buf, tok.data(), k);
		qaa_guest_store8(g, buf + (uint32_t)k, 0);
		return tok.empty() ? 0 : 1;
	}
	if (c.trap == p.gamestate) {
		/* gameState_t: int stringOffsets[1024]; char stringData[]: offset 0 is "" */
		uint32_t gs = (uint32_t)c.args[0];
		const uint32_t max_data = 16000;
		if (!qaa_guest_is_mapped(g, gs, 0x1000 + max_data))
			return 0;
		std::lock_guard<std::mutex> l(gate.lock);
		uint32_t data = gs + 0x1000, used = 1;
		qaa_guest_store8(g, data, 0);
		for (auto &kv : gate.configstrings) {
			if (kv.first < 0 || kv.first >= 1024 || used + kv.second.size() + 1 > max_data)
				continue;
			qaa_guest_store32(g, gs + 4 * (uint32_t)kv.first, used);
			qaa_guest_write(g, data + used, kv.second.c_str(), kv.second.size() + 1);
			used += (uint32_t)kv.second.size() + 1;
		}
		return 0;
	}
	return 0;
}

/* ---- host services --------------------------------------------------------------------------- */

static void HostLog(void *, int level, const char *msg) {
	static const char *names[] = { "debug", "info", "warning", "error" };
	if (level == QAA_LOG_DEBUG && !getenv("QAA_TRACE_KERNEL"))
		return;
	printf("  [runtime %s] %s\n", names[level & 3], msg);
}

static std::string HostPath(const char *guest) {
	std::string p = guest;
	size_t colon = p.find(":\\");
	if (colon != std::string::npos)
		p = p.substr(colon + 2);
	for (char &ch : p)
		if (ch == '\\')
			ch = '/';
	return gate.files + "/" + p;
}

static int HostStat(void *, const char *path, qaa_file_info *info) {
	gate.file_log.push_back(std::string("stat ") + path);
	if (gate.files.empty())
		return -1;
	FILE *f = fopen(HostPath(path).c_str(), "rb");
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	info->size = ftell(f);
	info->is_directory = 0;
	fclose(f);
	return 0;
}

static int64_t HostOpen(void *, const char *path, int access, int disposition) {
	char b[48];
	snprintf(b, sizeof(b), " access %d disposition %d", access, disposition);
	gate.file_log.push_back(std::string("open ") + path + b);
	printf("  [file] open \"%s\"%s\n", path, b);
	if (gate.files.empty() || (access & QAA_FILE_WRITE))
		return -1;
	FILE *f = fopen(HostPath(path).c_str(), "rb");
	if (!f)
		return -1;
	gate.open_files.push_back(f);
	return (int64_t)gate.open_files.size() - 1;
}

static FILE *HostFile(int64_t h) {
	return h >= 0 && h < (int64_t)gate.open_files.size() ? gate.open_files[(size_t)h] : nullptr;
}

static int64_t HostRead(void *, int64_t h, int64_t off, void *buf, uint32_t len) {
	FILE *f = HostFile(h);
	if (!f)
		return -1;
	fseek(f, (long)off, SEEK_SET);
	return (int64_t)fread(buf, 1, len, f);
}

static int64_t HostSize(void *, int64_t h) {
	FILE *f = HostFile(h);
	if (!f)
		return 0;
	long cur = ftell(f);
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, cur, SEEK_SET);
	return size;
}

static void HostClose(void *, int64_t h) {
	if (FILE *f = HostFile(h)) {
		fclose(f);
		gate.open_files[(size_t)h] = nullptr;
	}
}

/* ---- report ------------------------------------------------------------------------------------ */

static void Summary(FILE *out, const Run &r) {
	fprintf(out, "%s: %zu calls, %zu distinct traps\n", r.module.c_str(), r.calls.size(), r.counts.size());
	std::map<int32_t, bool> seen;
	std::string order;
	for (auto &c : r.calls) {
		if (!seen[c.trap]) {
			seen[c.trap] = true;
			order += (order.empty() ? "" : " ") + TrapName(c.trap);
		}
	}
	fprintf(out, "  first-call order: %s\n", order.c_str());
	for (auto &kv : r.counts)
		fprintf(out, "  %-9s x%-6d first from %08X\n", TrapName(kv.first).c_str(), kv.second, r.first_lr.at(kv.first));
	fprintf(out, "  result: %s\n", r.result.c_str());
}

static void WriteJson(const char *path) {
	FILE *f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "{\n \"runs\": [\n");
	for (size_t ri = 0; ri < gate.runs.size(); ri++) {
		const Run &r = gate.runs[ri];
		fprintf(f, "  {\"module\": \"%s\", \"profile\": \"%s\", \"ok\": %s, \"result\": \"%s\", \"calls\": [\n",
			Escape(r.module).c_str(), r.profile->name, r.ok ? "true" : "false", Escape(r.result).c_str());
		for (size_t i = 0; i < r.calls.size(); i++) {
			const Call &c = r.calls[i];
			fprintf(f, "   {\"trap\": \"%s\", \"lr\": \"%08X\", \"args\": [", TrapName(c.trap).c_str(), c.lr);
			for (int k = 0; k < 8; k++)
				fprintf(f, "%s%d", k ? ", " : "", c.args[k]);
			std::string s = GuestString(gate.guest, (uint32_t)c.args[0], 256);
			fprintf(f, "], \"arg0_string\": \"%s\"}%s\n", Escape(s).c_str(), i + 1 < r.calls.size() ? "," : "");
		}
		fprintf(f, "  ]}%s\n", ri + 1 < gate.runs.size() ? "," : "");
	}
	fprintf(f, " ],\n \"files\": [");
	for (size_t i = 0; i < gate.file_log.size(); i++)
		fprintf(f, "%s\"%s\"", i ? ", " : "", Escape(gate.file_log[i]).c_str());
	fprintf(f, "]\n}\n");
	fclose(f);
}

/* ---- running a module --------------------------------------------------------------------------- */

static const Profile *ProfileFor(const std::string &kind, const char *path) {
	std::string k = kind;
	if (k == "auto")
		k = strstr(path, "cgame") ? "cgame" : strstr(path, "game") ? "game" : "none";
	return k == "game" ? &game_profile : k == "cgame" ? &cgame_profile : &no_profile;
}

static bool RunModule(const char *path, const Profile *profile, int command, const std::vector<int32_t> &init_args) {
	{
		std::lock_guard<std::mutex> l(gate.lock);
		gate.runs.push_back({});
		Current().module = path;
		Current().profile = profile;
	}
	Run &run = Current();
	char err[1024] = "";
	printf("== %s (stubs: %s)\n", path, profile->name);
	qaa_module *m = qaa_module_load(gate.guest, path, err, sizeof(err));
	if (!m) {
		run.result = std::string("load failed: ") + err;
		printf("FAIL: %s\n", run.result.c_str());
		return false;
	}
	printf("dllEntry (export %u at %08X)\n", QAA_EXPORT_DLLENTRY, qaa_module_export(m, QAA_EXPORT_DLLENTRY));
	if (qaa_module_dll_entry(m, Syscall, nullptr) != QAA_OK) {
		run.result = std::string("dllEntry failed: ") + qaa_guest_last_error(gate.guest);
		printf("FAIL: %s\n", run.result.c_str());
		return false;
	}
	if (profile->import_table_command >= 0) {
		const int entries = 128;
		uint32_t table = qaa_guest_alloc(gate.guest, 4 * entries);
		for (int i = 0; i < entries; i++)
			qaa_guest_store32(gate.guest, table + 4 * (uint32_t)i, qaa_module_host_function(m, ImportCall, (void *)(intptr_t)i));
		int32_t a0 = (int32_t)table;
		int64_t r = 0;
		printf("vmMain(%d, %08X): function table of %d logging stubs\n", profile->import_table_command, table, entries);
		if (qaa_module_vm_main(m, profile->import_table_command, &a0, 1, &r) != QAA_OK) {
			run.result = std::string("vmMain(function table) failed: ") + qaa_guest_last_error(gate.guest);
			printf("FAIL: %s\n", run.result.c_str());
			return false;
		}
	}
	printf("vmMain(%d", command);
	for (int32_t v : init_args)
		printf(", %d", v);
	printf(") (export %u at %08X)\n", QAA_EXPORT_VMMAIN, qaa_module_export(m, QAA_EXPORT_VMMAIN));
	int64_t result = 0;
	if (qaa_module_vm_main(m, command, init_args.data(), (int)init_args.size(), &result) == QAA_OK) {
		char b[64];
		snprintf(b, sizeof(b), "vmMain returned %lld", (long long)result);
		run.result = b;
		run.ok = true;
	} else {
		run.result = std::string("vmMain stopped: ") + qaa_guest_last_error(gate.guest);
	}
	printf("%s\n", run.result.c_str());
	return run.ok;
}

int main(int argc, char **argv) {
	int command = 0;
	double timeout = 60;
	const char *json = nullptr;
	const char *path = nullptr;
	const char *server = nullptr;
	std::string stubs = "auto";
	std::vector<int32_t> init_args;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&]() -> const char * {
			if (i + 1 >= argc) {
				fprintf(stderr, "%s needs a value\n", a.c_str());
				exit(2);
			}
			return argv[++i];
		};
		if (a == "--command") command = atoi(next());
		else if (a == "--server") server = next();
		else if (a == "--stubs") stubs = next();
		else if (a == "--error-trap") gate.error_trap = atoi(next());
		else if (a == "--max-traps") gate.max_traps = atoi(next());
		else if (a == "--print") gate.print = atoi(next());
		else if (a == "--timeout") timeout = atof(next());
		else if (a == "--entities") gate.entities = next();
		else if (a == "--files") gate.files = next();
		else if (a == "--json") json = next();
		else if (!path) path = argv[i];
		else init_args.push_back((int32_t)strtol(argv[i], nullptr, 0));
	}
	if (!path) {
		fprintf(stderr, "usage: qaa_gate [options] <module library> [init args...]\n");
		return 2;
	}
	setvbuf(stdout, nullptr, _IOLBF, 1 << 16);

	std::atomic<bool> done{ false };
	std::thread watchdog([&] {
		auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
		while (!done && std::chrono::steady_clock::now() < end)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		if (!done) {
			printf("TIMEOUT after %.0f s: the guest is still running\n", timeout);
			std::lock_guard<std::mutex> l(gate.lock);
			if (!gate.runs.empty())
				Current().result = "timeout";
			for (auto &r : gate.runs)
				Summary(stdout, r);
			if (json)
				WriteJson(json);
			fflush(stdout);
			_Exit(3);
		}
	});

	qaa_host host = {};
	host.log = HostLog;
	host.file_stat = HostStat;
	host.file_open = HostOpen;
	host.file_read = HostRead;
	host.file_size = HostSize;
	host.file_close = HostClose;
	char err[1024] = "";
	gate.guest = qaa_guest_create(&host, err, sizeof(err));
	bool ok = false;
	if (!gate.guest) {
		printf("FAIL: %s\n", err);
	} else {
		bool server_ok = true;
		if (server) {
			server_ok = RunModule(server, &game_profile, 0, { 0, 12345, 0 });
			printf("server set %zu configstrings\n", gate.configstrings.size());
		}
		if (server_ok)
			ok = RunModule(path, ProfileFor(stubs, path), command, init_args);
	}
	done = true;
	watchdog.join();
	printf("\n");
	for (auto &r : gate.runs)
		Summary(stdout, r);
	if (json)
		WriteJson(json);
	printf("GATE: %s\n", ok ? "vmMain returned" : "vmMain did not return");
	fflush(stdout);
	/* Guest threads may still run: leave without tearing the guest down under them. */
	_Exit(ok ? 0 : 1);
}
