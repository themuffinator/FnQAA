# Recompiler pipeline and guest runtime (M3)

QAA's game modules, `qagameXe.dll` and `cgameXe.dll`, are recompiled from
the user's package into native libraries at build time, on the user's
machine. A guest runtime hosts them: it gives them the Xbox 360 address
space they expect and the kernel functions they import, and lets the
engine call `dllEntry` and `vmMain`.

Facts below are marked as in [FORMATS.md](FORMATS.md): **verified** (the
build or a test checks it on the whole module), **code** (read from named
retail code) or **observed** (seen in a run).

## Building

```sh
python tools/recomp/build.py --package <package>
```

`<package>` is the STFS package, the zip that holds it, or a folder with
the extracted `qagameXe.dll` and `cgameXe.dll`. Everything goes to
`build/recomp/` (ignored by git):

| Path | What |
|---|---|
| `input/` | modules extracted from an STFS package (read-only copies) |
| `xenonrecomp/src`, `xenonrecomp/build` | patched XenonRecomp and its build |
| `<module>/<module>.toml`, `<module>_switch.toml` | generated XenonRecomp configs |
| `<module>/<module>_analysis.json` | analysis report: imports, exports, functions, jump tables, SEH, problems |
| `<module>/xenonrecomp.log`, `<module>_xenonanalyse.toml` | recompiler output and XenonAnalyse's own scan |
| `<module>/ppc/` | generated C++ |
| `<module>/glue/` | generated glue and the decoded image it embeds |
| `native/bin/` | `qaa_qagame.dll`, `qaa_cgame.dll`, `qaa_gate.exe`, `qaa_guest_tests.exe` |
| `summary.json` | per-module counts and every warning |

Requirements: Python 3 with `cryptography`, git, CMake, Ninja and clang
(clang-cl and lld-link on Windows; clang 19 or later for `#embed`).
`--strict` fails the build on any analysis problem or recompiler warning;
`--skip-native` stops after generating C++. **observed** A first full
build takes a few minutes on a 20-thread machine, almost all of it
compiling the generated C++; analysis and recompilation take seconds.

Tests, all headless:

```sh
python tools/recomp/tests/test_analyse.py        # analysis, synthetic PowerPC
build/recomp/native/bin/qaa_guest_tests          # runtime, synthetic module
build/recomp/native/bin/qaa_gate build/recomp/native/bin/qaa_qagame.dll 0 12345 0
build/recomp/native/bin/qaa_gate --server build/recomp/native/bin/qaa_qagame.dll \
    build/recomp/native/bin/qaa_cgame.dll 0 0 0
```

## The pipeline

1. **Inputs.** `build.py` finds the modules in the package (STFS through
   `tools/stfs.py`). Retail files are only read.
2. **XenonRecomp.** The commit pinned in
   [tools/recomp/xenonrecomp.json](../tools/recomp/xenonrecomp.json)
   (`ddd128b`, MIT) is exported with `git archive` from a local checkout
   (`--xenonrecomp`, `$QAA_XENONRECOMP`, or a `XenonRecomp` folder next to
   the repository), or cloned from GitHub when none is found. The patches in
   `tools/recomp/patches/` are applied to the export; the checkout itself
   is never modified. XenonRecomp and XenonAnalyse are built with CMake.
3. **Analysis** (`tools/recomp/analyse.py`, below) writes the TOML configs.
4. **Recompilation.** XenonAnalyse runs as a cross-check, then XenonRecomp.
   Its warnings, `// ERROR` lines in the output and any function it adds
   beyond the analysis are collected into `summary.json`.
5. **Glue** (`tools/recomp/glue.py`): one C++ file per module with the
   kernel import thunks, the module descriptor and the decoded image
   (embedded with `#embed`).
6. **Native build** (`tools/recomp/CMakeLists.txt`): the runtime
   (`code/guest`), one shared library per module, the gate and the tests.

### Feeding XenonRecomp

**verified** XenonRecomp's own XEX loader reads the retail modules
directly: AES with the retail key and basic compression. No intermediate
XEX is needed. Patch 0001 sizes its basic-compression buffer to the full
image (the stored blocks end at 0x78000 of qagame's 0x1C0000), and skips
the `.edata`/`.idata` section headers that lie past the image.

### What the analysis generates

XenonRecomp takes `.pdata` functions itself and sweeps the gaps with its
own boundary analysis, which splits functions at jump tables. The analysis
removes that guesswork: every byte of each code section is assigned to
exactly one of the following:

- **`.pdata` functions** (qagame 655, cgame 570).
- **Functions outside `.pdata`** (qagame 170, cgame 147): leaf functions,
  split at every reference (bl targets, `lis`/`addi` pairs and data words
  that point into code, exports, the entry point) and after a terminator
  that no branch reaches past. These become explicit `functions` entries.
- **Chunks** (qagame 13, cgame 11): code inside a `.pdata` function that
  another function branches into. **code** All of them are the bodies of
  `__finally` handlers, which the normal path calls with `bl` and which
  the unwind entry reaches with `b` (for example qagame `0x88055544` in
  `0x880553F8`). Each becomes an overlapping function that runs to the end
  of its parent.
- **Import thunks** (16 bytes each, named `__imp__<name>` by XenonRecomp),
  the register save/restore helpers (found by byte pattern; cgame has no
  VMX helpers) and skipped data. Skipped data is zero padding, the 8-byte
  `__C_specific_handler` / scope-table records that precede each SEH
  function, and the XEX export table at the end of `.text`.

The analysis then checks that every branch leaving a function lands on a
function start, that every jump-table label lies in its function, and that
no word is left uncovered. **verified** For both modules XenonRecomp adds
no function beyond the analysis.

**Jump tables.** **verified** XenonAnalyse finds none in these modules.
Its patterns expect `lis; addi; rlwinm; lwzx`, but this compiler emits
`lis; slwi; addi; lwzx` and puts `nop`s inside the offset forms. The
analysis instead emulates each `bctr`'s dispatch sequence. It takes the
`cmplwi crN, rI, K` / `bgt crN` bounds check and runs the straight-line
code to `mtctr` for every index 0..K, reading tables from the image. That
handles absolute, halfword-offset and shifted byte-offset tables in any
instruction order. It also handles an index the compiler recomputes after
the check (`addi r10, r11, -1; cmplwi r10; ... addi r11, r11, -1`).
qagame has 13 tables and cgame 18; every other `bctr` is an import thunk
or a tail call through a loaded pointer (qagame 6, cgame 11), and none is
left unresolved.

### XenonRecomp patches

| Patch | Change |
|---|---|
| `0001-xex-map-full-basic-compressed-image` | allocate the whole image for basic compression; do not map section headers past the image |
| `0002-recompiler-instructions-switch-fallthrough-traps` | see below |

0002:

- **New instructions** (every unsupported one in these modules): `bdzf`,
  `eqv`, `addme`, `subfze[.]`, `lhzu`, `sthu`, `lfsu`, `stfsu`. `bdnzf`
  now decodes its condition bit instead of assuming `eq`. **code** The
  compiler emits `mtctr rX; cmpwi crN, rX, 0; bdzf crN.eq, case1; bdzf
  crN.eq, case2; ...; bne crN, default` as a compact switch (qagame 33 and
  cgame 20 `bdzf`).
- **Switches on the 32-bit index.** The bounds check is `cmplwi`, so only
  the low word is meaningful. Upstream switches on the full 64-bit
  register and makes the default case unreachable.
- **Fall-through.** A function whose last instruction can fall through now
  calls the function that starts there, as the guest would. Explicit
  boundaries can split code anywhere. If the next word is padding, which
  happens after a call that does not return, the code calls `PPC_TRAP` as
  the hardware would raise an illegal-instruction exception.
- **Traps.** `tw`/`td` immediate forms (divide-by-zero checks, debug
  breaks) call `PPC_TRAP(address)` when their condition holds, if the
  macro is defined. Upstream drops them, so a division by zero would run
  into the host's division fault and a debug break would do nothing.

**verified** With the patches, neither module produces any recompiler
warning (`summary.json`).

## The guest runtime

`code/guest/` holds the runtime (C++20, built as a static library). Its
public API is C ([qaa_guest.h](../code/guest/include/qaa_guest.h)) and
knows nothing about the engine.

### Modules

A module library exports one function, `qaa_module_describe`. It returns
the image (base, size, entry point, embedded data), the recompiler's
function mappings, the imports with their slot and thunk addresses, the
exports, and a `bind` hook
([qaa_module_abi.h](../code/guest/include/qaa_module_abi.h)). Module
libraries link nothing from the runtime. Kernel calls, the indirect-call
table and trap reporting arrive through `bind`, so one runtime hosts both
modules in one address space. The runtime checks the module's ABI version
and `sizeof(PPCContext)` and XenonRecomp's layout switches (all
optimisations are off, so the context keeps every register).

`qaa_module_load` does the following:

- maps the stored image at its base and zero-fills the rest (BSS);
- registers every recompiled function in the indirect-call table;
- fills each import slot: kernel variables get the address of an HLE
  object, functions get their thunk address (which resolves to the glue's
  `__imp__` function);
- binds the module;
- runs the entry point, `_DllMainCRTStartup(hModule, DLL_PROCESS_ATTACH,
  0)`, where `hModule` is a fake `LDR_DATA_TABLE_ENTRY`.

**Exports.** **code** Ordinal 1 is `vmMain` and ordinal 2 is `dllEntry`,
the reverse of the guess in FORMATS.md: qagame's 0x88042158 starts a
13-way `switch (command)`, and 0x8804C990 stores r3 and returns. cgame is
the same at 0x86020E68 (16 cases) and 0x86031D88.

**Indirect calls** (`ppc_detail.h`, injected with `PPC_INCLUDE_DETAIL`)
go through a two-level table, `page[addr >> 16][(addr & 0xFFFF) >> 2]`,
shared by all modules. A miss goes to the runtime. If the address is a
host-call address, the runtime calls the host. Otherwise the call fails
as "no recompiled function".

**Calls into the guest** (`qaa_module_dll_entry`, `qaa_module_vm_main`,
`qaa_guest_call`) set r3 to r10, write further arguments to the caller's
parameter area (r1 + 0x50, 8 bytes each), and use the thread's guest
stack. A call made from inside a syscall (guest to host to guest) saves the
whole context and builds its frame below the caller's red zone. The host's
MXCSR is restored afterwards, because the generated code toggles
flush-to-zero for VMX.

**Syscalls.** `dllEntry` receives a host-call address. A guest call to it
reaches the host's `qaa_syscall_fn` with the trap number (r3), the
arguments r4 to r10, nine stack slots (read at r1 + 0x54 + 8k, the low
word), f1 to f13 and the call site. The value it returns goes to r3. The
callback may call back into the guest, or call `qaa_guest_abort` to
abandon the guest call, as `Com_Error` does.
`qaa_module_host_function` makes more such addresses, for the function
tables the engine passes (see the gate).

**Memory helpers:** translation (`qaa_guest_translate`, `qaa_guest_base`),
big-endian loads and stores of 8, 16, 32 and 64 bits and of floats,
copies, `strnlen`, a mapped check, and a guest heap for the host
(`qaa_guest_alloc`).

### Guest memory map

The runtime reserves 4 GiB of host address space; guest address `a` is
host `base + a`. Pages are committed on use.

| Guest range | Use |
|---|---|
| `00000000-0000FFFF` | never mapped (null guard) |
| `40000000-6FFFFFFF` | `NtAllocateVirtualMemory` arena: 64 KiB reservations, 4 KiB commits |
| `70000000-7DFFFFFF` | thread stacks: 1 MiB for host threads, the requested size (at least 256 KiB) for guest threads, with 64 KiB unmapped guards |
| `7E000000-7FFFFFFF` | kernel pool: PCRs, KTHREADs, LDR entries, kernel variables, string buffers, `qaa_guest_alloc` |
| `80000000-8FFFFFFF` | module images at their bases: cgame `86000000-863A0000`, qagame `88000000-881C0000` (`82000000` is free for `default.xex` later) |
| `9F000000-9F0FFFFF` | host-call addresses (never mapped; reached only by indirect calls) |

### Kernel HLE

`code/guest/src/kernel.cpp` implements all 55 imports of the two modules.
Calls resolve by ordinal. Any other ordinal, or any xam call, fails the
guest call by name.

- **Files** (`NtCreateFile`, `NtOpenFile`, `NtReadFile`, `NtWriteFile`,
  `NtQueryInformationFile`, `NtSetInformationFile`,
  `NtQueryFullAttributesFile`, `NtQueryVolumeInformationFile`,
  `NtFlushBuffersFile`, `NtClose`, `NtDuplicateObject`) go through the
  `qaa_host` callbacks (`file_open`, `file_stat`, `file_read`,
  `file_write`, `file_size`, `file_close`). Guest paths are passed
  unchanged, for the engine to map onto its filesystem. The runtime keeps
  each handle's file position. **code** The modules reach these functions
  only through a CRT dispatch table in `.data` (qagame `0x88071044`), never
  with a direct call.
- **Memory**: `NtAllocateVirtualMemory`, `NtFreeVirtualMemory` and
  `NtQueryVirtualMemory` over the arena. `MEM_TOP_DOWN` is honoured.
- **Threads**: `ExCreateThread` runs a host thread with its own guest
  stack, PCR and KTHREAD. It calls the XAPI start-up routine (or the start
  address) and honours `CREATE_SUSPENDED` and `NtResumeThread`.
  `ExTerminateThread` unwinds the thread. Waits on a thread handle complete
  when the thread ends. `KeSetAffinityThread`,
  `ObReferenceObjectByHandle` (which returns the KTHREAD) and
  `ObDereferenceObject` are also implemented.
- **Synchronisation**: events (`NtCreateEvent`, `NtSetEvent`,
  `NtClearEvent`); `NtWaitForSingleObjectEx` and
  `NtWaitForMultipleObjectsEx` with any, all and timeout; critical
  sections on the guest's own `RTL_CRITICAL_SECTION` fields (owner =
  KTHREAD, recursion), so code that reads them sees real values.
- **TLS**: `KeTlsAlloc`, `KeTlsFree` and `KeTlsGetValue`/`SetValue`, 64
  slots per guest thread. **code** The modules read r13 directly: PCR +
  0x100 (current KTHREAD) and PCR + 0x150. Through the thread they read
  KTHREAD + 0x14C (thread id) and + 0x160 (last error). The runtime sets
  these up for every thread.
- **Strings**: `RtlInitAnsiString`, `RtlInitUnicodeString`,
  `RtlUnicodeStringToAnsiString`, `RtlFreeAnsiString`,
  `RtlMultiByteToUnicodeN`, `RtlCompareMemoryUlong` and
  `RtlNtStatusToDosError`.
- **Time and system**: `KeQuerySystemTime` (FILETIME) and
  `KeQueryPerformanceFrequency`. The recompiler turns `mftb` into
  `rdtsc` (one qagame function uses it), so the frequency is the host
  TSC's, measured at start-up. `KeGetCurrentProcessType` returns 1 (title).
- **Variables**: `XboxHardwareInfo` (6 hardware threads, hard disk
  present), `KeDebugMonitorData` (none), `ExThreadObjectType` and
  `XexExecutableModuleHandle`. The last points to a fake executable entry
  whose XEX header has no optional headers. **code** The CRT asks
  `RtlImageXexHeaderField` for key 0x20401 (default heap size), and with no
  answer uses its built-in default.
- **Errors**: `KeBugCheck`, `KeBugCheckEx` and `RtlRaiseException`
  abandon the guest call with the code (see below).

Set `QAA_TRACE_KERNEL=1` to log every kernel call.

### Exceptions

XenonRecomp cannot dispatch guest exceptions: there is no unwinder over
recompiled frames. What the modules need from SEH:

- **verified** The SEH functions (qagame 19, cgame 18) all lie in the last
  part of `.text`, around the CRT entry point: qagame 0x880553F8 to
  0x88063648 (entry 0x880556F0), cgame 0x86044508 to 0x86051958 (entry
  0x86044800). That is the statically linked runtime, not game code (an
  inference from their placement and the calls below).
- **verified** 14 of qagame's and 13 of cgame's have only `__finally`
  scopes (target 0); 5 in each have an `__except` scope. **code** The
  compiler emits `__finally` handlers as funclets, which the normal path
  calls with `bl` (the chunks above), so the normal path is recompiled and
  runs correctly.
- **code** The only `RtlRaiseException` calls (qagame 2, cgame 1) raise
  `STATUS_NO_MEMORY` (0xC0000017) after a failed allocation when the heap
  flag 0x4 (`HEAP_GENERATE_EXCEPTIONS`) is set.
- **code** Neither module imports `RtlUnwind` or has a `setjmp`/`longjmp`,
  so XenonRecomp's setjmp redirection is not needed.

The runtime's behaviour is as follows:

- `RtlRaiseException` ends the current guest call with the exception code
  (MSVC's thread-naming exception 0x406D1388 is ignored).
- `__C_specific_handler` is never expected to run, and fails if it does.
- Hardware faults in guest code (access violations, division faults) are
  caught with SEH (Windows; `sigsetjmp` and signals elsewhere). The call
  that faulted returns `QAA_ERROR` with the guest address and the nearest
  recompiled function.
- Trap instructions do the same through `PPC_TRAP`.

Guest state after such a failure is whatever the guest left. On the Xbox
these cases would crash the game.

## The gate (M3)

`qaa_gate` (`code/guest/tests/gate.cpp`) is headless. It loads a module,
calls `dllEntry` with a stub engine that logs every trap with its
arguments, calls `vmMain(INIT)` and reports which traps were called before
it returned or failed. Stubs return 0. String-buffer traps get "", and
`MILLISECONDS` returns real time. The game's entity-token trap serves one
synthetic `worldspawn`, and with `--server` cgame's gamestate trap serves
the configstrings that qagame's INIT set. Run with these stubs, both
inits return.

**observed** qagame, `vmMain(0 = GAME_INIT, 0, 12345, 0)`, returns 0
after 93 calls:

| Order | Trap | Calls | Seen as |
|---|---|---|---|
| 1 | 0 | 6 | print: init banner, game name and date, team and item counts |
| 2 | 3 | 57 | cvar register (vmCvar_t *, name, default, flags) |
| 3 | 19 | 1 | get configstring (num, buffer, 1024) |
| 4 | 18 | 9 | set configstring (num, string) |
| 5 | 7 | 1 | cvar string buffer ("session", buffer, 1024) |
| 6 | 15 | 9 | locate game data (gentities, count, size, clients, size) |
| 7 | 38 | 5 | get entity token (buffer, 1024): Quake III has this as 37 |
| 8 | table +0x14 | 3 | cvar set (name, value) |
| 9 | table +0x18 | 2 | (name, ...): cvar query, by its arguments |

**observed** cgame, `vmMain(0 = CG_INIT, 0, 0, 0)` after qagame, returns 0
after 690 calls. First-call order: 3 (cvar register, x118), 7
(`r_widescreen`), 5 (cvar set), 6 (cvar string buffer), 16 (add command,
x78), 52 (glconfig), 53 (gamestate), 19 (load map), table +0x04 (register
sound, x170), table +0x00 (x70), 42, 18, 38, table +0x14 (x89), table
+0x0C (x120), table +0x18 (x9), 50 (x12), 20, 37, 82 (x8), 31.

What the runs show about QAA's engine interface (input for M4):

- **observed** Traps 0 to 6 follow Quake III's legacy numbers in both
  modules. After that they differ: qagame's entity-token trap is 38 (Q3
  37); in cgame, 7 is a cvar query, add command is 16 (Q3 15) and load map
  is 19 (Q3 18).
- **code** Besides the syscall pointer, each module takes an engine
  function table through `vmMain`. qagame's command 9 stores its argument
  at 0x881B0710; cgame's command 12 stores it at 0x8627D824. Init calls
  through these tables (cvar set, sound registration and more), so the
  engine must pass the table before INIT. The gate passes 128 logging
  stubs.
- **code** cgame's `gameState_t` holds 1024 string offsets, with string
  data at +0x1000.
- **observed** Neither init opens a file through the kernel, and cgame's
  helper thread is not started during init.

## Known gaps

- **Trap and function-table numbering** is only what the gate observed.
  The real tables are M4's work (with the ABI recovery in `docs/ABI.md`).
- **Directory listing**: `NtQueryDirectoryFile` returns no entries, and
  the host interface has no directory callback yet. `NtReadFileScatter` is
  not implemented, and writes cannot resize a file through the host
  interface. The modules call none of these at init.
- **XEX header queries** get no answer, because `default.xex` is not
  loaded, so the CRT uses its defaults (heap size). Real values could come
  from the decoded `default.xex` headers.
- **Exceptions**: no guest SEH dispatch (above). An out-of-memory raise or
  a fault ends the call instead of running a guest handler.
- **POSIX**: the runtime has a POSIX path (mmap, dlopen, signals) but has
  only been built and run on Windows.
- **Performance**: XenonRecomp's optimisations (`skip_lr`, registers as
  locals) are off. They would also change `PPCContext`'s layout, and the
  runtime checks that layout at load.
- **Threads in the gate**: the unit tests cover `ExCreateThread`, events and
  waits on the synthetic module. The gate has not run cgame's helper
  thread.
- **Host stack**: guest recursion runs on the host stack (one native frame
  per guest frame).
