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
 *     --abi DIR       trap and engine-table names from the data/abi tables
 *                     (default: the repository's data/abi, if present)
 *     --xex-header F  default.xex's XEX header (default: the build's
 *                     qaa_default_xex_header.bin next to the module, if any)
 *     --settle MS     after INIT, keep the process alive MS milliseconds
 *                     for guest threads (default 1000)
 *     --error-trap N  trap that ends the call as an engine error (default 1)
 *     --max-traps N   stop after N traps (default 100000)
 *     --print N       print the first N traps in full (default 400)
 *     --timeout S     give up after S seconds (default 60)
 *     --entities S    entity string for the game's entity-token trap
 *                     (default one worldspawn: synthetic, not from a map)
 *     --files DIR     serve guest paths ("game:\x" -> DIR/x) read-only,
 *                     with directory listing
 *     --json FILE     write the trap sequence as JSON
 *     --call SPEC     after INIT, call a guest function: ADDR[:ARG...], each
 *                     ARG a number or @K, the result of the K-th --call
 *                     (repeatable; for probing module code directly)
 *
 * Stubs: every trap and engine-table call returns 0 unless the profiles
 * below say otherwise. Trap numbers are QAA's, from the ABI recovery
 * (docs/ABI.md, data/abi game_traps.tsv and cgame_traps.tsv). Buffers are only written when the
 * argument points at mapped guest memory and the size is plausible.
 */
#include "qaa_guest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef QAA_ABI_DIR
#define QAA_ABI_DIR ""
#endif

/* ---- stub profiles (QAA's numbers, docs/ABI.md) --------------------------------------- */

struct BufferStub {
	int trap, buffer_arg, size_arg;	/* indices into qaa_syscall::args (args[0] = first argument) */
};

enum TableStub { T_NONE, T_CVAR_SET, T_CVAR_INTEGER, T_FS_FOPEN, T_FS_READ, T_WAIT };

struct Profile {
	const char *name;
	const char *abi_module;					/* "game" / "cgame" in data/abi */
	std::vector<BufferStub> buffers;		/* string-buffer traps: give "" */
	int millis, cvar_register, fopen, set_configstring, entity_token, gamestate;
	int engine_table_command;				/* vmMain(N, table) before INIT */
	std::map<int, TableStub> table;			/* engine-table byte offset -> stub */
};

/* G_MILLISECONDS 2, G_CVAR_REGISTER 3, G_CVAR_VARIABLE_STRING_BUFFER 7,
 * G_ARGV 9, G_FS_FOPEN_FILE 10, G_SET_CONFIGSTRING 18, G_GET_CONFIGSTRING
 * 19, G_GET_USERINFO 20, G_GET_SERVERINFO 22, G_GET_ENTITY_TOKEN 38;
 * vmMain 9 = QAA_GAME_SET_ENGINE_TABLE; table +0x14 Cvar_Set, +0x18
 * Cvar_VariableIntegerValue. */
static const Profile game_profile = {
	"game", "game",
	{ { 7, 1, 2 }, { 9, 1, 2 }, { 19, 1, 2 }, { 20, 1, 2 }, { 22, 0, 1 } },
	2, 3, 10, 18, 38, -1, 9,
	{ { 0x14, T_CVAR_SET }, { 0x18, T_CVAR_INTEGER } },
};

/* CG_MILLISECONDS 2, CG_CVAR_REGISTER 3, CG_CVAR_VARIABLESTRINGBUFFER 6,
 * CG_ARGV 9, CG_ARGS 10, CG_FS_FOPENFILE 11, CG_GETGAMESTATE 53; vmMain 12
 * = QAA_CG_SET_ENGINE_TABLE; table +0x20 FS_FOpenFileByMode, +0x24 FS_Read,
 * +0x40 an engine event wait (used by the helper thread). */
static const Profile cgame_profile = {
	"cgame", "cgame",
	{ { 6, 1, 2 }, { 9, 1, 2 }, { 10, 0, 1 } },
	2, 3, 11, -1, -1, 53, 12,
	{ { 0x20, T_FS_FOPEN }, { 0x24, T_FS_READ }, { 0x40, T_WAIT } },
};

static const Profile no_profile = { "none", "", {}, -1, -1, -1, -1, -1, -1, -1, {} };

/* ---- state ----------------------------------------------------------------------------- */

enum { IMPORT_BASE = 100000 };	/* engine-table calls: IMPORT_BASE + byte offset */

struct Call {
	int32_t trap;
	int32_t args[QAA_SYSCALL_MAX_ARGS];
	uint32_t lr;
	bool other_thread;
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
	std::map<std::string, std::string> cvars;
	std::map<std::string, std::string> trap_names;		/* "game:3" -> G_CVAR_REGISTER */
	std::map<std::string, std::string> table_names;		/* "game:0x14" -> Cvar_Set ... */
	std::thread::id main_thread = std::this_thread::get_id();
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

static Gate gate;

static Run &Current() { return gate.runs.back(); }

/* ---- helpers ------------------------------------------------------------------------------ */

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

static std::string TrapName(const Profile *p, int32_t t) {
	char b[32];
	std::string key, name;
	if (t >= IMPORT_BASE) {
		snprintf(b, sizeof(b), "0x%02X", t - IMPORT_BASE);
		key = std::string(p->abi_module) + ":" + b;
		auto it = gate.table_names.find(key);
		name = it != gate.table_names.end() ? it->second : "";
		snprintf(b, sizeof(b), "imp+0x%02X", t - IMPORT_BASE);
	} else {
		key = std::string(p->abi_module) + ":" + std::to_string(t);
		auto it = gate.trap_names.find(key);
		name = it != gate.trap_names.end() ? it->second : "";
		snprintf(b, sizeof(b), "%d", t);
	}
	return name.empty() ? std::string(b) : std::string(b) + " " + name;
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

static size_t Record(Call &c) {
	c.other_thread = std::this_thread::get_id() != gate.main_thread;
	std::lock_guard<std::mutex> l(gate.lock);
	Run &r = Current();
	r.calls.push_back(c);
	if (!r.counts[c.trap]++)
		r.first_lr[c.trap] = c.lr;
	return r.calls.size();
}

static void PrintCall(qaa_guest *g, const Call &c, size_t n) {
	if ((int)n > gate.print)
		return;
	std::string label = (c.trap >= IMPORT_BASE ? "import " : "trap ") + TrapName(Current().profile, c.trap);
	printf("  %s%-40s lr %08X  args%s\n", c.other_thread ? "[thread] " : "", label.c_str(), c.lr, Describe(g, c, 8).c_str());
}

/* vmCvar_t (QAA, 0x110 bytes): handle, modificationCount, value, integer, string[256] */
static void FillVmCvar(qaa_guest *g, uint32_t vmcvar, const std::string &value, int handle) {
	if (!vmcvar || !qaa_guest_is_mapped(g, vmcvar, 0x110))
		return;
	qaa_guest_store32(g, vmcvar + 0x0, (uint32_t)handle);
	qaa_guest_store32(g, vmcvar + 0x4, 1);
	qaa_guest_storef(g, vmcvar + 0x8, (float)atof(value.c_str()));
	qaa_guest_store32(g, vmcvar + 0xC, (uint32_t)atoi(value.c_str()));
	std::string v = value.substr(0, 255);
	qaa_guest_write(g, vmcvar + 0x10, v.c_str(), v.size() + 1);
}

/* ---- the stub engine ------------------------------------------------------------------------ */

static int64_t EngineTableCall(void *user, qaa_module *m, const qaa_syscall *sc) {
	qaa_guest *g = qaa_module_guest(m);
	int offset = 4 * (int)(intptr_t)user;
	Call c;
	c.trap = IMPORT_BASE + offset;
	c.args[0] = sc->trap;	/* a host function's first argument (r3) */
	memcpy(c.args + 1, sc->args, sizeof(c.args) - sizeof(c.args[0]));
	c.lr = sc->return_address;
	size_t n = Record(c);
	PrintCall(g, c, n);
	if ((int)n >= gate.max_traps)
		qaa_guest_abort(g, "trap limit reached");
	auto it = Current().profile->table.find(offset);
	switch (it == Current().profile->table.end() ? T_NONE : it->second) {
	case T_CVAR_SET: {		/* (name, value) */
		std::string name = GuestString(g, (uint32_t)c.args[0], 256), value = GuestString(g, (uint32_t)c.args[1], 256);
		std::lock_guard<std::mutex> l(gate.lock);
		gate.cvars[name] = value;
		return 0;
	}
	case T_CVAR_INTEGER: {	/* (name) */
		std::string name = GuestString(g, (uint32_t)c.args[0], 256);
		std::lock_guard<std::mutex> l(gate.lock);
		auto v = gate.cvars.find(name);
		return v == gate.cvars.end() ? 0 : atoi(v->second.c_str());
	}
	case T_FS_FOPEN: {		/* (path, fileHandle_t *, mode): not found */
		uint32_t h = (uint32_t)c.args[1];
		if (h && qaa_guest_is_mapped(g, h, 4))
			qaa_guest_store32(g, h, 0);
		return -1;
	}
	case T_FS_READ:
		return 0;
	case T_WAIT:			/* the helper thread's idle wait: do not spin */
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		return 0;
	default:
		return 0;
	}
}

static int64_t Syscall(void *, qaa_module *m, const qaa_syscall *sc) {
	qaa_guest *g = qaa_module_guest(m);
	const Profile &p = *Current().profile;
	Call c;
	c.trap = sc->trap;
	memcpy(c.args, sc->args, sizeof(c.args));
	c.lr = sc->return_address;
	size_t n = Record(c);
	PrintCall(g, c, n);
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
	if (c.trap == p.cvar_register) {	/* (vmCvar_t *, name, default, flags) */
		std::string name = GuestString(g, (uint32_t)c.args[1], 256), def = GuestString(g, (uint32_t)c.args[2], 256);
		std::string value;
		int handle;
		{
			std::lock_guard<std::mutex> l(gate.lock);
			auto it = gate.cvars.find(name);
			if (it == gate.cvars.end())
				it = gate.cvars.emplace(name, def).first;
			value = it->second;
			handle = (int)std::distance(gate.cvars.begin(), it) + 1;
		}
		FillVmCvar(g, (uint32_t)c.args[0], value, handle);
		return 0;
	}
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

/* ---- host services ------------------------------------------------------------------------------ */

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
	std::error_code ec;
	std::filesystem::path p = HostPath(path);
	if (!std::filesystem::exists(p, ec))
		return -1;
	info->is_directory = std::filesystem::is_directory(p, ec);
	info->size = info->is_directory ? 0 : (int64_t)std::filesystem::file_size(p, ec);
	return 0;
}

static int HostDirEntry(void *, const char *path, uint32_t index, char *name, size_t size, qaa_file_info *info) {
	if (index == 0)
		gate.file_log.push_back(std::string("list ") + path);
	if (gate.files.empty())
		return -1;
	std::error_code ec;
	std::vector<std::filesystem::directory_entry> entries;
	for (auto &e : std::filesystem::directory_iterator(HostPath(path), ec))
		entries.push_back(e);
	std::sort(entries.begin(), entries.end(), [](auto &a, auto &b) { return a.path().filename() < b.path().filename(); });
	if (index >= entries.size())
		return -1;
	snprintf(name, size, "%s", entries[index].path().filename().string().c_str());
	info->is_directory = entries[index].is_directory(ec);
	info->size = info->is_directory ? 0 : (int64_t)entries[index].file_size(ec);
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

/* ---- ABI names ------------------------------------------------------------------------------------ */

static std::vector<std::vector<std::string>> ReadTsv(const std::string &path) {
	std::vector<std::vector<std::string>> rows;
	std::ifstream f(path);
	std::string line;
	std::getline(f, line);	/* header */
	while (std::getline(f, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		std::vector<std::string> cols;
		std::stringstream ss(line);
		std::string col;
		while (std::getline(ss, col, '\t'))
			cols.push_back(col);
		rows.push_back(cols);
	}
	return rows;
}

static bool LoadAbiNames(const std::string &dir) {
	if (dir.empty() || !std::filesystem::exists(dir + "/game_traps.tsv"))
		return false;
	for (const char *m : { "game", "cgame" })
		for (auto &r : ReadTsv(dir + "/" + m + "_traps.tsv"))	/* num, qaa_name, ... */
			if (r.size() >= 2)
				gate.trap_names[std::string(m) + ":" + r[0]] = r[1];
	for (auto &r : ReadTsv(dir + "/engine_tables.tsv"))		/* module, table, offset, function */
		if (r.size() >= 4) {
			/* "S_RegisterSound(name, 0) 0x82174370": drop the engine address */
			std::string fn = r[3];
			size_t sp = fn.rfind(" 0x");
			if (sp != std::string::npos && fn.find(' ', sp + 1) == std::string::npos)
				fn.resize(sp);
			gate.table_names[r[0] + ":" + r[2]] = fn;
		}
	return true;
}

/* ---- report ---------------------------------------------------------------------------------------- */

static void Summary(FILE *out, const Run &r) {
	size_t threaded = std::count_if(r.calls.begin(), r.calls.end(), [](const Call &c) { return c.other_thread; });
	fprintf(out, "%s: %zu calls (%zu from guest threads), %zu distinct\n", r.module.c_str(), r.calls.size(), threaded,
		r.counts.size());
	std::map<int32_t, bool> seen;
	std::string order;
	for (auto &c : r.calls) {
		if (!seen[c.trap]) {
			seen[c.trap] = true;
			char b[32];
			snprintf(b, sizeof(b), c.trap >= IMPORT_BASE ? "imp+0x%02X" : "%d", c.trap >= IMPORT_BASE ? c.trap - IMPORT_BASE : c.trap);
			order += (order.empty() ? "" : " ") + std::string(b);
		}
	}
	fprintf(out, "  first-call order: %s\n", order.c_str());
	for (auto &kv : r.counts)
		fprintf(out, "  %-40s x%-6d first from %08X\n", TrapName(r.profile, kv.first).c_str(), kv.second, r.first_lr.at(kv.first));
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
			fprintf(f, "   {\"trap\": \"%s\", \"thread\": %s, \"lr\": \"%08X\", \"args\": [",
				Escape(TrapName(r.profile, c.trap)).c_str(), c.other_thread ? "true" : "false", c.lr);
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

/* ---- running a module ---------------------------------------------------------------------------------- */

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
	if (profile->engine_table_command >= 0) {
		const int entries = 32;
		uint32_t table = qaa_guest_alloc(gate.guest, 4 * entries);
		for (int i = 0; i < entries; i++)
			qaa_guest_store32(gate.guest, table + 4 * (uint32_t)i, qaa_module_host_function(m, EngineTableCall, (void *)(intptr_t)i));
		int32_t a0 = (int32_t)table;
		int64_t r = 0;
		printf("vmMain(%d, %08X): engine function table of %d stubs\n", profile->engine_table_command, table, entries);
		if (qaa_module_vm_main(m, profile->engine_table_command, &a0, 1, &r) != QAA_OK) {
			run.result = std::string("vmMain(engine table) failed: ") + qaa_guest_last_error(gate.guest);
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

static std::vector<uint8_t> ReadFile(const std::string &path) {
	std::ifstream f(path, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main(int argc, char **argv) {
	int command = 0, settle_ms = 1000;
	double timeout = 60;
	const char *json = nullptr;
	const char *path = nullptr;
	const char *server = nullptr;
	std::string stubs = "auto", abi = QAA_ABI_DIR, xex_header;
	std::vector<std::string> calls;
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
		else if (a == "--abi") abi = next();
		else if (a == "--xex-header") xex_header = next();
		else if (a == "--settle") settle_ms = atoi(next());
		else if (a == "--error-trap") gate.error_trap = atoi(next());
		else if (a == "--max-traps") gate.max_traps = atoi(next());
		else if (a == "--print") gate.print = atoi(next());
		else if (a == "--timeout") timeout = atof(next());
		else if (a == "--entities") gate.entities = next();
		else if (a == "--files") gate.files = next();
		else if (a == "--json") json = next();
		else if (a == "--call") calls.push_back(next());
		else if (!path) path = argv[i];
		else init_args.push_back((int32_t)strtol(argv[i], nullptr, 0));
	}
	if (!path) {
		fprintf(stderr, "usage: qaa_gate [options] <module library> [init args...]\n");
		return 2;
	}
	setvbuf(stdout, nullptr, _IOLBF, 1 << 16);
	printf("ABI names: %s\n", LoadAbiNames(abi) ? abi.c_str() : "none (data/abi not found)");
	if (xex_header.empty()) {
		std::filesystem::path p = std::filesystem::path(path).parent_path() / "qaa_default_xex_header.bin";
		if (std::filesystem::exists(p))
			xex_header = p.string();
	}

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
	host.dir_entry = HostDirEntry;
	char err[1024] = "";
	gate.guest = qaa_guest_create(&host, err, sizeof(err));
	bool ok = false;
	if (!gate.guest) {
		printf("FAIL: %s\n", err);
	} else {
		if (!xex_header.empty()) {
			std::vector<uint8_t> h = ReadFile(xex_header);
			int rc = qaa_guest_set_executable_header(gate.guest, h.data(), h.size());
			printf("executable XEX header: %s (%zu bytes)%s\n", xex_header.c_str(), h.size(), rc == QAA_OK ? "" : " REJECTED");
		} else {
			printf("executable XEX header: none\n");
		}
		bool server_ok = true;
		if (server) {
			server_ok = RunModule(server, &game_profile, 0, { 0, 12345, 0 });
			printf("server set %zu configstrings\n", gate.configstrings.size());
		}
		if (server_ok)
			ok = RunModule(path, ProfileFor(stubs, path), command, init_args);
		std::vector<int64_t> results;
		for (size_t ci = 0; ok && ci < calls.size(); ci++) {
			std::vector<int64_t> v;
			std::stringstream ss(calls[ci]);
			std::string tok;
			while (std::getline(ss, tok, ':')) {
				if (!tok.empty() && tok[0] == '@') {
					size_t k = (size_t)atoi(tok.c_str() + 1);
					v.push_back(k < results.size() ? results[k] : 0);
				} else {
					v.push_back((int64_t)strtoll(tok.c_str(), nullptr, 0));
				}
			}
			if (v.empty())
				continue;
			int64_t r = 0;
			int rc = qaa_guest_call(gate.guest, (uint32_t)v[0], v.data() + 1, (int)v.size() - 1, &r);
			results.push_back(r);
			printf("call %zu: %s -> %s %lld (0x%llX)\n", ci, calls[ci].c_str(), rc == QAA_OK ? "returned" : "FAILED",
				(long long)r, (unsigned long long)(uint32_t)r);
			if (rc != QAA_OK)
				printf("  %s\n", qaa_guest_last_error(gate.guest));
		}
		if (ok && settle_ms > 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(settle_ms));
			printf("settled %d ms for guest threads\n", settle_ms);
		}
	}
	done = true;
	watchdog.join();
	printf("\n");
	{
		std::lock_guard<std::mutex> l(gate.lock);
		for (auto &r : gate.runs)
			Summary(stdout, r);
		if (json)
			WriteJson(json);
	}
	printf("GATE: %s\n", ok ? "vmMain returned" : "vmMain did not return");
	fflush(stdout);
	/* Guest threads may still run: leave without tearing the guest down under them. */
	_Exit(ok ? 0 : 1);
}
