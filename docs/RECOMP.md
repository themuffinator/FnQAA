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
the extracted files. Everything goes to `build/recomp/` (ignored by git):

| Path | What |
|---|---|
| `input/` | `qagameXe.dll`, `cgameXe.dll` and `default.xex` extracted from an STFS package (read-only copies) |
| `xenonrecomp/src`, `xenonrecomp/build` | patched XenonRecomp and its build |
| `<module>/<module>.toml`, `<module>_switch.toml` | generated XenonRecomp configs |
| `<module>/<module>_analysis.json` | analysis report: imports, exports, functions, jump tables, SEH, problems |
| `<module>/xenonrecomp.log`, `<module>_xenonanalyse.toml` | recompiler output and XenonAnalyse's own scan |
| `<module>/ppc/` | generated C++ |
| `<module>/glue/` | generated glue, the decoded image and the module's XEX header it embeds |
| `qaa_default_xex_header.bin` | `default.xex`'s XEX header, for the runtime |
| `native/bin/` | the module libraries, `qaa_gate`, `qaa_guest_tests`, the synthetic test module, and a copy of the header |
| `summary.json` | per-module counts and every warning |

Requirements: Python 3 with `cryptography`, git, CMake, Ninja and clang
(clang-cl and lld-link on Windows). The glue uses `#embed` where the
compiler has it (clang 19 and later) and a generated initializer list
otherwise. `--strict` fails the build on any analysis problem or
recompiler warning; `--skip-native` stops after generating C++. **observed**
A first full build takes a few minutes on a 20-thread machine, almost all
of it compiling the generated C++; analysis and recompilation take
seconds.

**verified** Windows (clang-cl 20) and Linux (Ubuntu clang 18, under WSL)
both build the pipeline end to end, pass the unit tests and run both gates
to completion.

Tests, all headless:

```sh
python tools/recomp/tests/test_analyse.py        # analysis, synthetic PowerPC
build/recomp/native/bin/qaa_guest_tests build/recomp/native/bin/qaa_synthetic_module.dll
build/recomp/native/bin/qaa_gate build/recomp/native/bin/qaa_qagame.dll 0 12345 0
build/recomp/native/bin/qaa_gate --server build/recomp/native/bin/qaa_qagame.dll \
    build/recomp/native/bin/qaa_cgame.dll 0 0 0
```

On Linux the libraries are `libqaa_qagame.so` and so on.

### Meson

The engine builds with Meson. `code/guest/meson.build` is written to be
included from the top-level build:

```meson
qaa_xenonrecomp_src = meson.project_source_root() / 'build/recomp/xenonrecomp/src'
subdir('code/guest')
# then link qaa_guest_dep
```

It defines `qaa_guest_dep`, `qaa_gate` and `qaa_guest_tests`, and registers
the unit tests with `meson test`. **verified** It builds and passes with
clang-cl on Windows. It needs clang, and builds the runtime as C++20
whatever the project's standard is. The module libraries still come from
`build.py`.

## The pipeline

1. **Inputs.** `build.py` finds the modules and `default.xex` in the
   package (STFS through `tools/stfs.py`). Retail files are only read.
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
   kernel import thunks, the module descriptor, the exception-dispatch
   tables, the decoded image and the module's XEX header.
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

**Exception sites** (`[[seh_site]]` entries; see "Exceptions" below). The
analysis finds every function that can reach `RtlRaiseException` through
calls, and every `__except` scope that encloses such a call. It then marks
each call site between them where an exception may unwind or resume:
qagame 23 sites, cgame 21. For each function that may be popped it also
records, from its prologue, where non-volatile registers were saved
(qagame 10 functions, cgame 10, all decoded). It decodes `std`/`stfd`
saves and the `__savegprlr`/`__savefpr` helpers relative to the caller's
stack pointer, using the prologue length in `.pdata`.

### XenonRecomp patches

| Patch | Change |
|---|---|
| `0001-xex-map-full-basic-compressed-image` | allocate the whole image for basic compression; do not map section headers past the image |
| `0002-recompiler-instructions-switch-fallthrough-traps` | see below |
| `0003-recompiler-seh-resume-sites` | `[[seh_site]]` config entries: after such a call, emit a `PPC_SEH_RESUME(site)` check that returns from the function (1) or jumps to the given `__except` label (2) |
| `0004-recompiler-addc-stfdu-vpkswss` | `addc`, `stfdu`, `vpkswss` and `vpkswss128`, which `default.xex`'s GFx code uses ([subset modules](#subset-modules)) |

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

**XenonRecomp's optimisations stay off**, by decision rather than as a
gap. They turn registers into C locals (`non_volatile_as_local`,
`cr_as_local` and so on). That is only sound when every guest function is
one C function. Here the analysis splits code at references and
fall-through calls continue in the next piece, so a register live across a
split would be lost. Exception unwinding also restores registers into the
context. Both need every register in `PPCContext`.

## Subset modules

The front end (`ui`, [UI.md](UI.md)) is not a module of its own on the
console: it is part of `default.xex`, the engine. `build.py` recompiles the
whole image into `ppc_full/` (XenonRecomp is given every function, so jump
tables and references resolve) and then keeps a subset, configured by
`data/recomp/ui.json`:

- **roots**: address ranges whose functions are wanted (SFUI and its
  renderer);
- **libraries**: ranges that are followed when reached (GFx, zlib, the CRT,
  the XAM and XGraphics helpers) but not wanted for their own sake;
- **root_tables**: data ranges whose code pointers are roots (the CRT
  initializer table);
- **recompile**: single functions outside those ranges to keep anyway (the
  CRT start-up, small helpers GFx shares with the engine);
- **switches**: jump tables the analysis cannot bound by itself
  (`decode_switch_override` in `analyse.py`; zlib's `inflate` checks its
  state at the bottom of its loop).

`tools/recomp/select_funcs.py` takes the closure of the roots through `bl`,
`b` and `bc` targets, `lis`/`addi` code references and the code pointers in
data tables (up to the next label, or 1024 words). Functions in a root or
library range, or listed in `recompile`, are kept. Every other function
the kept code reaches becomes a **hook**. `tools/recomp/subset.py` filters
the generated `ppc_recomp.*.cpp` files and the function mapping down to
the kept functions. `glue.py` emits each hook as
`PPC_FUNC(sub_X) { qaa_module_indirect_miss(ctx, base, 0xX); }`, so a
call reaches the host's hook for that address (below). It sets the entry
point to 0, since the engine's own `main` must not run; the host starts
the CRT itself. XAM imports are named from the kernel's `xam_table.inc`.

**observed** The ui module keeps about 6,350 of the image's 17,558
functions, and about 80 engine functions are hooks.

## The guest runtime

`code/guest/` holds the runtime (C++20, built as a static library). Its
public API is C ([qaa_guest.h](../code/guest/include/qaa_guest.h)) and
knows nothing about the engine.

### Modules

A module library exports one function, `qaa_module_describe`. It returns
the image (base, size, entry point, embedded data), the module's XEX
header, the recompiler's function mappings, the imports with their slot
and thunk addresses, the exports, the exception-dispatch tables, and a
`bind` hook ([qaa_module_abi.h](../code/guest/include/qaa_module_abi.h),
ABI 3). Module libraries link nothing from the runtime. Kernel calls, the
indirect-call table, trap reporting and SEH resumption arrive through
`bind`, so one runtime hosts both modules in one address space. The
runtime checks the module's ABI version and `sizeof(PPCContext)` and
XenonRecomp's layout switches.

`qaa_module_load` does the following:

- maps the stored image at its base and zero-fills the rest (BSS);
- registers every recompiled function in the indirect-call table;
- fills each import slot: kernel variables get the address of an HLE
  object, functions get their thunk address (which resolves to the glue's
  `__imp__` function);
- binds the module;
- runs the entry point, `_DllMainCRTStartup(hModule, DLL_PROCESS_ATTACH,
  0)`, where `hModule` is an `LDR_DATA_TABLE_ENTRY` whose XEX header is
  the module's own.

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
`qaa_module_host_function` makes more such addresses, for the engine
function tables the modules take through `vmMain`.

**Executable header.** `qaa_guest_set_executable_header` loads
`default.xex`'s XEX header (the build writes it to
`qaa_default_xex_header.bin`). The modules reach it through
`XexExecutableModuleHandle` and `RtlImageXexHeaderField`. **code** The CRT
asks for key 0x20401 (default heap size). **verified** `default.xex` has no
such header, so the answer, 0, is the console's, and the CRT uses its
built-in default.

**Memory helpers:** translation (`qaa_guest_translate`, `qaa_guest_base`),
big-endian loads and stores of 8, 16, 32 and 64 bits and of floats,
copies, `strnlen`, a mapped check, and a guest heap for the host
(`qaa_guest_alloc`).

**Threads.** Recompiled code uses one native frame per guest frame, often
larger than the guest's, so guest-created threads get a 16 MiB (reserved)
host stack. A host thread that calls into the guest should have a stack of
that order too. On Windows that means linking the engine with a larger
`/STACK`, or calling from a thread created with one.
`qaa_guest_destroy` leaves the guest allocated, with a warning, while a
guest thread is still running (cgame's worker parks for good).

### Guest memory map

The runtime reserves 4 GiB of host address space; guest address `a` is
host `base + a`. Pages are committed on use.

| Guest range | Use |
|---|---|
| `00000000-0000FFFF` | never mapped (null guard) |
| `40000000-6FFFFFFF` | `NtAllocateVirtualMemory` arena: 64 KiB reservations, 4 KiB commits |
| `70000000-7DFFFFFF` | guest thread stacks: 1 MiB for host threads, the requested size (at least 256 KiB) for guest threads, with 64 KiB unmapped guards |
| `7E000000-7FFFFFFF` | kernel pool: PCRs, KTHREADs, LDR entries, XEX headers, kernel variables, string buffers, `qaa_guest_alloc` |
| `80000000-8FFFFFFF` | module images at their bases: cgame `86000000-863A0000`, qagame `88000000-881C0000` (`82000000` is free for `default.xex` later) |
| `9F000000-9F0FFFFF` | host-call addresses (never mapped; reached only by indirect calls) |

### Kernel HLE

`code/guest/src/kernel.cpp` implements all 55 imports of the two modules.
Calls resolve by ordinal. Any other ordinal, or any xam call, fails the
guest call by name.

- **Files** (`NtCreateFile`, `NtOpenFile`, `NtReadFile`, `NtWriteFile`,
  `NtQueryInformationFile`, `NtSetInformationFile`,
  `NtQueryFullAttributesFile`, `NtQueryVolumeInformationFile`,
  `NtQueryDirectoryFile`, `NtFlushBuffersFile`, `NtClose`,
  `NtDuplicateObject`) go through the `qaa_host` callbacks: `file_open`,
  `file_stat`, `file_read`, `file_write`, `file_size`, `file_set_size`,
  `file_close`, and `dir_entry` for listings. The runtime matches the
  guest's wildcard pattern and returns one `FILE_DIRECTORY_INFORMATION` per
  call. Guest paths are passed unchanged, for the engine to map onto its
  filesystem; the runtime keeps each handle's file position. **code** The
  modules reach these functions only through a CRT dispatch table in
  `.data` (qagame `0x88071044`), never with a direct call.
  `NtReadFileScatter` is the one file call not implemented; it is listed in
  that table but nothing calls it.
- **Memory**: `NtAllocateVirtualMemory`, `NtFreeVirtualMemory` and
  `NtQueryVirtualMemory` over the arena. `MEM_TOP_DOWN` is honoured.
  **observed** The modules' own CRT heap runs on it: `malloc`, `realloc`
  growth and shrinking work, and a 1 GiB `realloc` fails cleanly with NULL
  (`qaa_gate --call`).
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
  `XexExecutableModuleHandle` (see "Executable header").
- **Errors**: `KeBugCheck` and `KeBugCheckEx` abandon the guest call with
  their code. `RtlRaiseException` dispatches the exception (next section).

Set `QAA_TRACE_KERNEL=1` to log every kernel call.

The front end imports far more than the game modules, so the kernel
also has:

- **Kernel variables**: `ExEventObjectType`; a `KeTimeStampBundle` that
  is kept current on every kernel call, guest call and hook; and
  `XboxKrnlVersion`. An unknown variable gets a zeroed block.
- **Unimplemented imports**: these go to the host's import hooks. Without
  one, a kernel function returns `STATUS_UNSUCCESSFUL` and a XAM function
  returns 0, so that, for example, `XexLoadImage` does not appear to
  succeed.
- **Large pages**: an `MEM_LARGE_PAGES` allocation commits whole 64 KiB
  pages, and decommitting leaves them committed (**observed**: otherwise
  the CRT heap faults at 0x41501002 during the front end's start-up).

### Host hooks and lockstep threads

For subset modules ([above](#subset-modules)), `qaa_guest_set_hook` gives a
non-recompiled guest address a host implementation. The handler gets a
`qaa_hook_call` with the address, the call site, the stack pointer,
r3..r10 and f1..f13, and writes its result to r3 (`r[0]`) or f1 (`f[0]`). A
call to an address that has no hook goes to the default hook, if one is set.
`qaa_guest_set_import_hook` does the same for imports
(`library << 16 | ordinal`). Handlers may call into the guest again.
`qaa_guest_call_regs` calls a guest function with up to eight integer and
13 float arguments and returns r3 and f1.

A guest thread can be run in lockstep with the engine.
`qaa_guest_step_thread(thread, event)` signals an event the thread waits on,
then blocks until the thread waits on it again (or exits).
`qaa_guest_wait_thread_blocked` waits for that state without signalling.
The front end's update thread advances one frame per step this way
([UI.md](UI.md#frames-input-and-events)).

### Exceptions

What the modules use SEH for:

- **verified** The SEH functions (qagame 19, cgame 18) all lie in the last
  part of `.text`, around the CRT entry point: qagame 0x880553F8 to
  0x88063648 (entry 0x880556F0), cgame 0x86044508 to 0x86051958 (entry
  0x86044800). That is the statically linked runtime, not game code (an
  inference from their placement and the calls below).
- **verified** 14 of qagame's and 13 of cgame's have only `__finally`
  scopes; 5 in each have an `__except` scope. **code** The compiler emits
  `__finally` handlers as funclets, which the normal path calls with `bl`
  (the chunks above).
- **code** The only `RtlRaiseException` calls (qagame 2, cgame 1) raise
  `STATUS_NO_MEMORY` (0xC0000017) after a failed heap allocation when heap
  flag 0x4 (`HEAP_GENERATE_EXCEPTIONS`) is set.
- **code** The `__except` filters come in four kinds:
  - catch-all (`li r3, 1`): qagame 0x8805A89C, 0x88062A44 and 0x88063F18;
    cgame 0x860484B4 and 0x8604F624;
  - `code == STATUS_NO_MEMORY`: qagame 0x8805A520, cgame 0x86048200;
  - `code != STATUS_NO_MEMORY`: qagame 0x88063EF4;
  - calls into a CRT filter routine: qagame 0x880556C0, cgame 0x860447D0
    and 0x860519B4.
- **code** The reallocator at qagame `0x88063648` wraps its own raise, and
  its call to the allocator `0x88062A58` (which holds the other raise), in
  an `__except` with the `!=` filter. Faults while it walks the heap get
  NULL as the result (handler 0x88063E6C), but its own `STATUS_NO_MEMORY`
  goes on up. No frame above any raise site accepts it, so on the console
  an out-of-memory raise would be unhandled.
- **code** Neither module imports `RtlUnwind` or has a `setjmp`/`longjmp`,
  so XenonRecomp's setjmp redirection is not needed.

The runtime dispatches guest exceptions the way the Xbox kernel does, in
two phases over the guest stack.

- **Search.** It walks the frames from the raise. Each frame's return
  address is the LR its callee saved at (caller SP - 8), and its caller's
  SP is the back chain at 0(SP). For every `__except` scope around a
  frame's call, innermost first, it runs the filter funclet with r3 =
  `EXCEPTION_POINTERS` and r12 = the frame's caller SP (the establisher
  frame, as the compiled code passes it).
- **Unwind.** Once a filter accepts, it runs the `__finally` funclets of
  the frames being left (abnormal termination, r3 = 1), restores the
  non-volatile registers their prologues saved, and sets r1 to the handler
  frame's.
- **Resume.** The patch-0003 checks after each call on the way out return
  from every unwound frame and `goto` the handler label in the handler's
  frame.

`EXCEPTION_CONTINUE_EXECUTION` returns from the raise. A raise that no
filter accepts fails the guest call as "unhandled", as the title would
crash on the console. The filters really run: whether an exception is
caught is the retail code's decision, not a table's. **verified** The unit
tests cover the full sequence on a synthetic three-frame stack: filter,
`__finally` with its establisher frame, register restore, handler, and an
unhandled raise.

Two limits remain:

- A resume site knows one handler label, its innermost `__except`. If an
  inner filter declines and an outer scope in the same function accepts,
  the dispatch fails as unsupported. No such nesting is on the raise paths
  found.
- **observed** No run so far raises a real exception: the CRT paths
  exercised do not set `HEAP_GENERATE_EXCEPTIONS`.

Outside this, a hardware fault in guest code (an access violation, a
division fault) or a trap instruction fails the current guest call. The
error names the guest address and the nearest recompiled function. Faults
are caught with SEH on Windows and with signals elsewhere. They are not
offered to guest `__except` filters (see the known gaps).

## The gate (M3)

`qaa_gate` (`code/guest/tests/gate.cpp`) is headless. It loads a module,
passes the engine function table the module takes through `vmMain`, calls
`dllEntry` with a stub engine that logs every trap with its arguments, and
calls `vmMain(INIT)`. It then reports which traps and table entries were
called before INIT returned or failed.

Trap numbers in the stubs are QAA's, from the ABI recovery
([ABI.md](ABI.md); the gate prints their names when it finds the
`data/abi` tables, or use `--abi DIR`). The stub engine does the following:

- returns 0 for every call unless listed here;
- writes "" into string-buffer traps;
- fills the `vmCvar_t` (0x110 bytes) on cvar registration with the
  default value and answers cvar queries from what was registered or set;
- returns real time for `MILLISECONDS`;
- serves one synthetic `worldspawn` through the entity-token trap;
- fails file opens;
- with `--server`, gives cgame's gamestate trap the configstrings that
  qagame's INIT set.

`--call ADDR:ARG...` calls module functions directly after INIT, for
probing.

**observed** qagame, `vmMain(GAME_INIT, 0, 12345, 0)`, returns 0 after 95
calls. In first-call order:

| Trap | Calls | |
|---|---|---|
| 0 `G_PRINT` | 6 | init banner, game name and date, team and item counts |
| 3 `G_CVAR_REGISTER` | 57 | |
| 19 `G_GET_CONFIGSTRING` | 1 | |
| 18 `G_SET_CONFIGSTRING` | 10 | |
| 22 `G_GET_SERVERINFO` | 1 | |
| 7 `G_CVAR_VARIABLE_STRING_BUFFER` | 1 | `session` |
| 15 `G_LOCATE_GAME_DATA` | 9 | |
| 38 `G_GET_ENTITY_TOKEN` | 5 | |
| table +0x14 `Cvar_Set` | 3 | |
| table +0x18 `Cvar_VariableIntegerValue` | 2 | |

**observed** cgame, `vmMain(CG_INIT, 0, 0, 0)` after qagame in the same
guest, returns 0 after 690 calls. First-call order:

- 3 `CG_CVAR_REGISTER` (x118)
- 7 `QAA_CG_CVAR_VARIABLEVALUE`
- 5 `CG_CVAR_SET`
- 6 `CG_CVAR_VARIABLESTRINGBUFFER`
- 16 `CG_ADDCOMMAND` (x78)
- 52 `CG_GETGLCONFIG`
- 53 `CG_GETGAMESTATE`
- 19 `CG_CM_LOADMAP`
- table +0x04 `S_RegisterSound(name, 1)` (x170)
- table +0x00 `S_RegisterSound(name, 0)` (x70)
- 42 `CG_R_CLEARSCENE`
- 18 `CG_UPDATESCREEN`
- 38 `CG_R_LOADWORLDMAP`
- table +0x14 `RegisterShader` (x89)
- table +0x0C `RegisterModel` (x120)
- table +0x18 `RegisterShaderNoMip` (x9)
- 50 `CG_R_MODELBOUNDS` (x12)
- 20 `CG_CM_NUMINLINEMODELS`
- 37 `CG_S_STARTBACKGROUNDTRACK`
- 82 `CG_R_REMAP_SHADER` (x8)
- 31 `CG_S_CLEARLOOPINGSOUNDS`

**observed** At the end of INIT, cgame starts its model-loading worker,
because `cg_asyncModelLoading` defaults to 1:

- `ExCreateThread` (XAPI start-up 0x86051958, entry 0x8601E650) creates
  it suspended;
- `KeSetAffinityThread` moves it to hardware thread 2, and
  `NtResumeThread` resumes it;
- the thread runs its CRT thread set-up, then parks in
  `NtWaitForMultipleObjectsEx` on its 64 request events (wait-any,
  alertable).

That is its idle state until the cgame queues a load. Neither init opens a
file through the kernel.

What this shows about QAA's engine interface, consistent with the ABI
recovery: traps 0 to 6 follow Quake III's legacy numbers, and later ones
are offset. Each module also takes an engine function table through
`vmMain` (qagame command 9, stored at 0x881B0710; cgame command 12, at
0x8627D824) before INIT. cgame's `gameState_t` holds 1024 string offsets,
with string data at +0x1000.

## Known gaps

- **The trap bridge** is M4. The gate's stubs are just enough for both
  inits to run and return.
- **Hardware faults are not guest exceptions.** An access violation in
  guest code fails the call. It is not offered to an `__except` filter.
  Resuming means continuing a recompiled function in the middle of the
  faulting statement, which the generated code cannot do. On the console
  the catch-all and CRT filters listed above could catch such a fault.
  **code** The catch-all scopes are small regions of the heap code
  (qagame 0x880624F0 to 0x88062510 and 0x88063CC4 to 0x88063CE4). Judging
  by where they are, they guard reads of caller-supplied heap blocks, so
  only a corrupted heap or a bad pointer from the game would fault there.
- **Unwinding** supports the GPR and FPR saves this compiler emits. A
  prologue that saves vector registers (none on the exception paths found)
  makes the analysis report a problem.
- **`NtReadFileScatter`** is not implemented (never called).
