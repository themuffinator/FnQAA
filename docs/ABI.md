# Engine ↔ game module ABI

How QAA's engine (`default.xex`, image at 0x82000000) loads and talks to its
two game modules, `qagameXe.dll` (0x88000000) and `cgameXe.dll`
(0x86000000). These are the facts the M4/M5 syscall bridge needs. The
machine-readable tables are in [`data/abi/`](../data/abi):

| File | Contents |
|---|---|
| `game_traps.tsv`, `cgame_traps.tsv` | Every trap number: QAA name, FnQL name and number, dispatch case address, engine function, argument types, return convention, number of module call sites |
| `vmmain.tsv` | vmMain commands for both modules, with engine callers and module handlers |
| `engine_tables.tsv` | The engine function tables QAA hands each module (QAA only, see below) |
| `structs.tsv` | Field offsets of every shared structure, QAA vs FnQL |
| `limits.tsv` | Array limits and sizes |

Each claim is marked **verified** (checked over a whole table or all uses),
**code** (read from the named retail function, by its address) or
**observed** (seen, not proven). Names of engine functions (`SV_LinkEntity`,
`Cvar_Register` ...) are identified by their role and strings against
Quake III and Quake Live source; the binaries carry no symbols. Names starting
`QAA_` are provisional names for QAA-only entries.

"FnQL" means the public headers in `E:\Repositories\FnQL` (`code/game/g_public.h`,
`code/cgame/cg_public.h`, `code/qcommon/q_shared.h`): Quake Live's syscall
numbering in its legacy (Quake III-style) form, plus FnQL's own extensions.
Retail Quake Live later replaced numbered syscalls with native function
tables; QAA predates that and uses numbered syscalls throughout, so the
legacy enums are the right comparison.

All guest structures are big-endian. Every multi-byte field crossing the
bridge needs byte-swapping, and every pointer is a 32-bit guest address.

## 1. Loading the modules

**code** `VM_Create` (0x8213C310) takes `(name, systemCalls, interpret)`, finds
a free `vm_t` (three slots of 0xCC bytes at 0x82DC3100) and calls `Sys_LoadDll`
(0x82184770). Both modules are always loaded as native DLLs.

| Module | Caller | systemCalls | vm global |
|---|---|---|---|
| game | `SV_InitGameProgs` 0x82134248 (`"vm_game"`, `"VM_Create on game failed"`) | `SV_GameSystemCalls` 0x82130438 | `gvm` 0x82C01960 |
| cgame | `CL_InitCGame` 0x821023F8 (`"vm_cgame"`, `"VM_Create on cgame failed"`) | `CL_CgameSystemCalls` 0x821008B0 | `cgvm` 0x82FF9400 |

**code** `Sys_LoadDll` (0x82184770) tries `game:\%sXe%s.dll` and
`game:\%sXe.dll` paths, loads the image (0x821F25D8), then resolves exports
**by ordinal** (0x821F2568):

- **ordinal 2 is `dllEntry`**, called at once with the syscall entry point;
- **ordinal 1 is `vmMain`**, stored in `vm->entryPoint`.

This reverses the guess in PLAN.md.

| Module | `vmMain` (ord 1) | `dllEntry` (ord 2) | syscall pointer global |
|---|---|---|---|
| qagame | 0x88042158 | 0x8804C990 | 0x88071BC4 |
| cgame | 0x86020E68 | 0x86031D88 | 0x86062488 |

**code** Both `dllEntry`s are three instructions that store `r3` in the
module's syscall pointer global and return. They have no `.pdata` entry.

**code** `vm_t` as QAA lays it out: `programStack` 0x00, `systemCall` 0x04,
`name[64]` 0x08, `dllHandle` 0x48, `entryPoint` 0x4C, `dataBase` 0x68,
`dataMask` 0x6C, `fqpath` 0x88. `currentVM` is 0x82C01964, `lastVM`
0x82C01968.

## 2. Calling conventions

QAA uses the Xbox 360 PowerPC ABI: integer and pointer arguments in r3..r10,
float arguments in f1..f13 (each also taking a parameter slot), further
arguments in 8-byte stack slots from 0x50(r1) at the call (low word
significant), results in r3 or f1.

### Syscalls (module → engine)

**code** `dllEntry` receives `VM_DllSyscall` (0x8213BEB8), not the
dispatcher. It is variadic: it stores r3 and the next 15 arguments (the low
word of each 8-byte slot) into an `int32 args[16]` on its stack and calls
`currentVM->systemCall(args)`. So:

- `args[0]` is the trap number, `args[1..15]` its arguments; no trap uses more
  than 13 (`BOTLIB_AAS_PREDICT_CLIENT_MOVEMENT`).
- The value returned in r3 is the dispatcher's 32-bit result.
- **code** Pointer arguments are converted with `VM_ArgPtr` (0x8213C560): 0
  stays NULL, otherwise `vm->dataBase + arg` for DLLs. `dataBase` is 0 for
  DLL modules, so a pointer argument is the guest address unchanged.
- **code** Float arguments travel as their IEEE bit pattern in the integer
  slot (`PASSFLOAT`): the module stores the float and reloads the word
  (e.g. cgame 0x86019EE8), and the dispatcher reads it back with `lfs` from
  `args[]`.
- **code** Float results: the **game** dispatcher returns a float result's bit
  pattern (`stfs; lwz`, e.g. trap 103 at 0x8213370C, botlib 306/502/503/540).
  The **cgame** dispatcher converts every float result to an int with
  `fctiwz` (truncation): traps 7, 103-108 and 111 (0x82100C1C, 0x82102084 ...).
  The modules never call cgame 103-111 (they use native math), and the cgame's
  own wrapper for trap 7 (0x86031D98) converts the integer back to float.

The module side calls `syscall(num, ...)` through its pointer global; the
call is inlined at every use (455 sites in qagame, 310 in cgame, **observed**
by a scan of `lwz` from the pointer global). `module_call_sites` in the trap
tables counts these per trap; 10 qagame and 15 cgame sites compute the number
and are not attributed.

### vmMain (engine → module)

**code** `VM_Call` (0x8213C608) is `VM_Call(vm_t *vm, int callnum, ...)`. It
sets `currentVM` (restoring the previous one afterwards), copies 16 variadic
`int32` arguments, and calls `vm->entryPoint(callnum, a0, ..., a15)`: callnum
in r3, a0..a6 in r4..r10, a7..a15 in stack slots. Its result is r3.

**code** Both `vmMain`s switch on r3 and pass at most **three** arguments
(r4..r6) to their handlers; nothing after a2 is read. Game `vmMain` returns
-1 for an unknown command; cgame calls `CG_Error("vmMain: unknown command %i")`.

## 3. vmMain commands

Full table: `vmmain.tsv`. Compared with FnQL's `gameExport_t` /
`cgameExport_t` (the Quake III numbering for these ranges).

### game (13 commands, **code**, vmMain 0x88042158)

| QAA | Name | Args | FnQL |
|---|---|---|---|
| 0 | GAME_INIT | levelTime, randomSeed, restart | 0 |
| 1 | GAME_SHUTDOWN | restart | 1 |
| 2 | GAME_CLIENT_CONNECT | clientNum, firstTime, isBot → guest `char *` or NULL | 2 |
| 3 | GAME_CLIENT_BEGIN | clientNum | 3 |
| 4 | GAME_CLIENT_USERINFO_CHANGED | clientNum | 4 |
| 5 | GAME_CLIENT_DISCONNECT | clientNum | 5 |
| 6 | GAME_CLIENT_COMMAND | clientNum | 6 |
| 7 | GAME_CLIENT_THINK | clientNum | 7 |
| 8 | GAME_RUN_FRAME | levelTime | 8 |
| 9 | **QAA_GAME_SET_ENGINE_TABLE** | table pointer | - |
| 10 | GAME_CONSOLE_COMMAND | → qboolean | 9 |
| 11 | **QAA_GAME_UPDATE_SCORES** | clientNum | - |
| 12 | BOTAI_START_FRAME | time → int | 10 |

Differences: 9 and 11 are QAA's, so Quake III's 9 and 10 move to 10 and 12.
FnQL's Quake Live additions 11..17 (`GAME_CAN_CLIENT_SEE_CLIENT` ...
`GAME_GET_CLIENT_SCORE`) do not exist. The engine calls 11 after every
`GAME_CLIENT_BEGIN` (0x8212EC90) and `GAME_CLIENT_DISCONNECT` (0x8212E8B8); the
handler sends the host name and scores to the menus (game trap 69).

### cgame (16 commands, **code**, vmMain 0x86020E68)

| QAA | Name | Args | FnQL |
|---|---|---|---|
| 0 | CG_INIT | serverMessageNum, serverCommandSequence, clientNum | 0 |
| 1 | CG_SHUTDOWN | | 1 |
| 2 | CG_CONSOLE_COMMAND | → qboolean | 2 |
| 3 | CG_DRAW_ACTIVE_FRAME | serverTime, stereoView, demoPlayback | 3 |
| 4 | CG_CROSSHAIR_PLAYER | → int | 4 |
| 5 | CG_LAST_ATTACKER | → int | 5 |
| 6 | CG_KEY_EVENT | key, down | 6 |
| 7 | CG_MOUSE_EVENT | dx, dy | 7 |
| 8 | (empty, returns 0) | | (8 = EVENT_HANDLING) |
| 9 | CG_EVENT_HANDLING | type | 8 |
| 10 | **QAA_CG_SCOREBOARD_ROWS** | list id (float) → rows; never called | - |
| 11 | **QAA_CG_SCOREBOARD_CELL** | list id (float), row, column → string; never called | - |
| 12 | **QAA_CG_SET_ENGINE_TABLE** | table pointer | - |
| 13 | QAA, never called | | - |
| 14 | **QAA_CG_MATCH_START** | | - |
| 15 | **QAA_CG_PLAYER_TALKING** | memberIndex, clientNum | - |

Differences: `CG_EVENT_HANDLING` moves from 8 to 9 (**code**: `CL_KeyEvent`
0x82106D20 clears `KEYCATCH_CGAME` and calls 9 with type 0, where Quake III
calls `CG_EVENT_HANDLING`). FnQL's Quake Live additions 9..18
(`CG_CHAT_DOWN` ... `CG_GET_PHYSICS_TIME`) do not exist. **observed** 10, 11
and 13 have no `VM_Call` site in `default.xex` (all 44 sites pass constant
command numbers, and nothing else calls `vm->entryPoint`). 10 and 11 form a
scoreboard data provider (row count and cell text for lists 5.0 = team 1,
6.0 = team 2, 11.0 = everyone; **code** 0x86025170, 0x860253B0), probably a
Scaleform interface the shipped engine no longer uses. 14 is called by the Live
lobby code (0x82176370) and puts up "FIGHT!"; 15 by the engine's voice handler
(0x8217C818) at most once a second for each talking session member.

## 4. Engine function tables (QAA only)

**code** Before `GAME_INIT` and `CG_INIT` the engine calls vmMain 9 (game)
and 12 (cgame) with the address of a table of **engine** function pointers.
Each module stores it (qagame 0x881B0710, cgame 0x8627D824) and from then on
calls those engine functions directly, by indirect call, instead of going
through `syscall`. This is why traps such as `G_LINKENTITY`, `G_TRACE` and
`G_CVAR_UPDATE` have no syscall sites in qagame.

| Module | Table | Filled by | Entries |
|---|---|---|---|
| game | 0x82E19DE0, 10 pointers | 0x82133858 (`SV_InitGameVM`) | UnlinkEntity, LinkEntity, Trace (with capsule arg), PointContents, Cvar_Update, Cvar_Set, Cvar_VariableIntegerValue, botlib AAS_PointContents, AAS_Initialized, BotLibUpdateEntity |
| cgame | 0x8300C7C0, 18 pointers (0x48 bytes) | 0x821023F8 (`CL_InitCGame`) | S_RegisterSound (two variants), S_UpdateEntityPosition, re.RegisterModel/Skin/Shader/ShaderNoMip, re.LerpTag, FS_FOpenFileByMode, FS_Read, FS_FCloseFile, five QAA-only renderer entries (a model-loading lock and three flag setters), two engine event helpers |

Slot-by-slot detail, signatures and the equivalent trap are in
`engine_tables.tsv`. For the bridge this means: the table must hold guest
addresses that the recompiled module's indirect calls resolve to host bridge
functions, with the same argument conventions as direct PowerPC calls (not
the `args[]` array). Two calls in the cgame table (re+0x1C/0x20) enter and
leave a renderer critical section around player-model loading, and two more
(0x82178240, 0x82178220) wait on and signal an engine event; both suggest
the cgame loads some media off the main thread (**observed**).

## 5. Trap tables

The dispatchers are dense jump tables: game `switch (args[0])` over 0..581
(table 0x8213046C, default 0x82133844), cgame over 0..142 (table 0x821008EC,
default 0x821023D8, `"Bad cgame system trap: %i"`). The game default has no
message of its own: it calls 0x826F9BC8 twice without setting arguments and
returns -1. Every case was traced (**code**, case addresses in the
TSVs); argument kinds are taken from each case's conversions (`VM_ArgPtr` =
pointer, `lfs` = float, plain load = int).

### Game (`game_traps.tsv`): 218 handled numbers

- **0..29: Quake III 1.32 / FnQL numbering.**
- **30 is QAA's** (`QAA_G_ACTIVE_CLIENT_COUNT`: counts clients that are
  neither free nor zombie). Quake III's 30..44 (`G_LINKENTITY` ...
  `G_ENTITY_CONTACTCAPSULE`) therefore sit at **31..45**.
- **46..72 are QAA's** Xbox traps (below). Quake III's 45, `G_FS_SEEK`, sits
  at **73**.
- 100..111 as Quake III's shared traps (`MEMSET` ... `CEIL`); 112/113
  (`TESTPRINTINT/FLOAT`) are not handled.
- **200..581: botlib, identical to Quake III 1.32 and FnQL** (`BOTLIB_SETUP`
  .. `BOTLIB_PC_SOURCE_FILE_AND_LINE`); each case calls the next slot of the
  botlib export table (0x82E19DC8), and argument conversions agree with
  Quake III's. The entity structures and the stubbed chat calls are QAA's
  own (section 6, botlib structures).
- Not present: FnQL/Quake Live's `G_STEAMID_QUERY`, `G_STEAM_AUTH_VALIDATE`,
  `G_RANK_*`, `G_CVAR_SETDESCRIPTION`, `G_TRAP_GETVALUE` (700) and the FnQL
  stats extensions.

Behavioural differences in shared traps (**code**):

- `G_SNAPVECTOR` (43) floors each component (`floorf`, 0x821DC8C8); Quake III
  rounds to nearest.
- `G_DEBUG_POLYGON_DELETE` (41) is a no-op.
- `G_CVAR_SET` uses `Cvar_Set2(..., force = 1)`.
- `G_DROP_CLIENT` ignores out-of-range client numbers instead of erroring.

QAA-only game traps (purpose from the engine side; the qagame call sites name
the context):

| # | Name | Args → result | What it does |
|---|---|---|---|
| 30 | QAA_G_ACTIVE_CLIENT_COUNT | → int | Clients whose state is above `CS_ZOMBIE` |
| 46 | QAA_G_MATCH_END | | Stops the active user's rumble; if trap 64 armed it, sets Live context 0xC = 2 and property 0x1000000C = 4 |
| 47 | QAA_G_AWARD_ACHIEVEMENT | userIndex, achievementId | Awards an achievement once per user (`AchievementAwarded` UI event), as cgame 114; unused |
| 48 | QAA_G_UI_GOTO_STACK_PATH | path | Menu navigation (`_global.sceneManager.gotoStackPath`); unused |
| 49 | QAA_G_UI_INGAME_MENU | int → -1 | Opens the in-game menu (`InvokeIngameMenu`); unused |
| 50, 51, 67, 68 | QAA_G_LIVE_SESSION_INFO_n | → int | Word n of a Live session-info record (0x82176AC8), 0 without a session; 50/67 used by the exit rules, 68 by the scoreboard |
| 52 | QAA_G_LIVE_SET_PROPERTY | value | Live worker sets property 0x1000000C = value; unused |
| 56, 57, 58 | QAA_G_LIVE_MEMBER_SET1 / SET0 / GET0 | member, value / member → int | Words 1 and 0 of a 16-byte per-member record that the engine replicates to the session peers (0x19-byte packets); unused |
| 61..63 | QAA_G_STUB_n | → 0 | Empty |
| 64 | QAA_G_MATCH_ARM_END | | If the session has reached the state behind event +0x38, arms trap 46 and makes the next `map_restart` a no-op; called by the exit rules |
| 65 | QAA_G_UI_COURSE_RESULTS | ptr (0x4C bytes) | Publishes single-player course results to the menus (`_global.courseResults`) |
| 66 | QAA_G_LIVE_WRITE_LEADERBOARD | viewId, value | Leaderboard write for the active user: one view, property 0x20000007 (int64) = value; unused |
| 69 | QAA_G_UI_CACHE_SCORES | hostName, scores | Menus' cached host name and scores |
| 70 | QAA_G_LIVE_SET_GAMEMODE | | Property 0x1000000C = 3, or 2 for a ranked session outside tournament; unused |
| 71 | QAA_G_LIVE_SET_PROPERTY_1 | | Property 0x1000000C = 1; called at `GAME_SHUTDOWN` |
| 72 | QAA_G_SEND_SCOREBOARD | data, size, reliable | Game-to-cgame scoreboard mailbox (read by cgame 141), also sent to Live peers |

"Unused" means qagame has no syscall site for it (**observed**). 53..55, 59
and 60 are unhandled.

### Cgame (`cgame_traps.tsv`): 135 handled numbers

- **0..6: Quake III / FnQL numbering.**
- **7 is QAA's** (`QAA_CG_CVAR_VARIABLEVALUE`, returns the value truncated to
  int). Quake III's 7..33 sit at **8..34**.
- **36 is QAA's** (`S_RegisterSound` with `compressed = 1`). Quake III's
  34..45 (`S_REGISTERSOUND` ... `R_SETCOLOR`) sit at **35..47**.
- **48 is unhandled.** Quake III's 46..89 (`R_DRAWSTRETCHPIC` ... `FS_SEEK`)
  sit at **49..92**.
- **93 is QAA's** (`QAA_CG_TURN_AROUND`: adds 180° to the client's yaw).
- 100..108 and 111 as Quake III's cgame math traps (`MEMSET` ... `CEIL`,
  `ACOS`); 109/110 are not handled. Floats come back truncated (section 2).
- **112..142 are QAA's** (below).
- Not present: FnQL's 90..95 key/binding extensions, the advertisement bridge,
  `CG_R_ADDREFENTITYTOSCENE2`, `CG_R_FORCEFIXEDDLIGHTS`,
  `CG_R_ADDLINEARLIGHTTOSCENE`, `CG_IS_RECORDING_DEMO`,
  `CG_CVAR_SETDESCRIPTION`, `CG_TRAP_GETVALUE`.

So most shared cgame traps are **renumbered** relative to FnQL (82 of them;
17 keep their number).

Behavioural differences in shared traps (**code**):

- `CG_R_DRAWSTRETCHPIC` (49) flips y: the renderer receives
  `y' = vidHeight - y - h` (vidHeight = `cls.glconfig` +0x2C2C, 0x82F88260).
- `CG_CIN_*` (77..81) and `CG_S_STOPLOOPINGSOUND` (84) are no-ops returning 0.
- `CG_S_CLEARLOOPINGSOUNDS` (31) ignores its argument and clears all loops.
- `CG_S_ADDLOOPINGSOUND` (32) drops velocity; `CG_S_RESPATIALIZE` (34) drops
  `inwater`; `CG_S_ADDREALLOOPINGSOUND` (83) is implemented as
  `S_StartSound` after a call to 0x821737B0.
- `CG_SNAPVECTOR` (74) floors, as the game trap.
- `CG_CM_BOXTRACE` and friends call a `CM_BoxTrace` with one extra pointer
  argument (0x82C0194C) before `brushmask`.
- `CG_GETUSERCMD` (58) copies 24 bytes (`usercmd_t`); `CG_GETGLCONFIG` (52)
  copies 0x2C54 bytes; `CG_GETGAMESTATE` (53) 0x4E84 bytes.

QAA-only cgame traps:

| # | Name | Args → result | What it does |
|---|---|---|---|
| 7 | QAA_CG_CVAR_VARIABLEVALUE | name → int | `Cvar_VariableValue`, truncated |
| 36 | QAA_CG_S_REGISTERSOUND_COMPRESSED | name | Registers a sound compressed; result discarded |
| 93 | QAA_CG_TURN_AROUND | | `cl.viewangles[YAW] += 180` |
| 112 | QAA_CG_STOP_RUMBLE | | Stops the active user's vibration; unused |
| 113 | QAA_CG_SET_RUMBLE | user, left %, left extra, right %, right extra | Controller vibration (`XamInputSetState`), only when focused and allowed by the profile |
| 114 | QAA_CG_AWARD_ACHIEVEMENT | userIndex, achievementId | Achievement award, as game 47 |
| 115 | QAA_CG_ACTIVE_USER_INDEX | → int | Active user / controller index |
| 116 | QAA_CG_UI_POSTGAME_PLAYER | ptr (0x48 bytes) | One player's record for the post-game menus, plus score per minute; called at shutdown |
| 117 | QAA_CG_UI_117 | 4 ptrs | Bulk copy (16 × 0x48 and 16 × 0x6B8 bytes) to the menus; unused |
| 118 | QAA_CG_UI_WAIT_BEGIN | | "Loading..." overlay; unused |
| 119 | QAA_CG_UI_HUD_DATA | ptr (0x108 bytes) | `_global.hudData`, every frame the HUD draws (**observed**) |
| 120 | QAA_CG_UI_CACHE_SCORES | hostName, scores | As game 69 |
| 121 | QAA_CG_UI_SCOREBOARD | int, ptr, int, ptr | Scoreboard blocks (0x6B80 and 0x480 bytes) to the menus |
| 122 | QAA_CG_UI_EVENT | name | Fires a menu event (`on<name>`) |
| 123, 124, 129, 131, 135 | QAA_CG_UI_* | various | Fixed menu events: `ItemPickup`, `Obituary`, `FollowingPlayer`, `PlayerVoiceInfo`, `Reward` |
| 126, 127, 128 | QAA_CG_UI_EVENT_S/SS/SSDD | name, strings | Menu event with formatted arguments (e.g. `CenterPrint "$fight"`) |
| 125, 130 | QAA_CG_UI_KILLER_STATS / TOURNAMENT | strings | `_global.statsKiller*`, `_global.tournament*` |
| 132 | QAA_CG_UI_SET_VALUE | index, value | `_global.uiValues[index]`; (0, 1) opens the in-game menu while `sys_titleFocused` < 1 (**observed**) |
| 133, 134 | QAA_CG_LIVE_MEMBER_TEST / STATE | member → int | Session-member predicates (134: 3 = the active local user, else 2/1/0 from two more predicates) shown in scoreboard columns 6 and 8 |
| 136, 137 | QAA_CG_R_8C / R_90 | | QAA renderer entries re+0x8C/+0x90; unused |
| 138, 139 | QAA_CG_LIVE_SESSION_INFO_2 / 0 | → int | As game 50; unused |
| 140 | QAA_CG_UI_140 | 3 ptrs | Menu call touching cvar `name`; unused |
| 141 | QAA_CG_GET_SCOREBOARD | buffer → int | Takes the 0x240-byte scoreboard from game 72's mailbox (or a Live peer packet); 1 if new |
| 142 | QAA_CG_R_RENDER_54 | ptr | Renderer entry re+0x54: queues render command 9 |

"Unused" means the cgame has no syscall site for it (**observed**); 119 and
132 do have sites (the cgame reaches them through a function pointer). The UI
traps feed QAA's Scaleform menus: the client hands them to its front end, game
48, 49, 65 and 69 as well ([UI.md](UI.md)). The Live traps can be stubbed on
the PC as documented PC deviations. How the engine itself hosts and joins
online matches is in [NETWORK.md](NETWORK.md).

**Coverage.** Every number the dispatchers handle has an entry: 218 game, 135
cgame. Shared traps are mapped to their Quake III/FnQL name with engine
function and argument types, and every QAA-only trap has its engine behaviour
recorded (**code**). What remains open: what the session-member predicates
behind cgame 133/134 test, what the per-member record words of game 56-58
mean, and the title-defined meaning of Live context 0xC and property
0x1000000C (the values are recorded). Live property and context ids are read
from the `XUSER_PROPERTY` / `XUSER_CONTEXT` record shapes the engine builds.

**The scoreboard mailbox works without Xbox Live.** Game 72 always copies its
data to 0x82A6D6E8 and raises a flag, and cgame 141 returns it to the cgame
whenever the flag is up, so a local or listen-server game relies on it: the
bridge must implement 72 and 141 as a game-to-cgame copy even when every other
Live trap is stubbed. qagame sends 16 records of 0x24 bytes (0x8803A3F0).

## 6. Shared structures

Full field tables in `structs.tsv` (one row per field: QAA offset, FnQL
offset, evidence, source address). FnQL offsets come from FnQL's headers
compiled for a 32-bit target, so both columns use 32-bit pointers.

| Struct | QAA | FnQL | Evidence | Source |
|---|---|---|---|---|
| `trajectory_t` | 0x24 | 0x28 | **verified** | netfield table 0x827EF130 |
| `entityState_t` | 0xD0 | 0xEC | **verified** fields, **code** size | 0x827EF130; `CL_GetSnapshot` 0x82100000 |
| `playerState_t` | 0x1D8 | 0x1F8 | **verified** networked fields, **code** size | 0x827EF3B0; 0x82100000 |
| `usercmd_t` | 0x18 | 0x1C | **code** | `MSG_ReadDeltaUsercmdKey` 0x82126FC8; 0x8213184C; 0x82101E64 |
| `sharedEntity_t` | 0x204 | 0x23C | **code** | `SV_LinkEntity` 0x82137DF8 |
| `entityShared_t` | 0x134 | 0x150 | **code** | 0x82137DF8, 0x821385A8, 0x82136F78 |
| `refEntity_t` | 0xC0 | 0x8C | **code** size, **observed** fields | `RE_AddRefEntityToScene` 0x821484E0 |
| `refdef_t` | 0x170 | 0x170 | **code** | `RE_RenderScene` 0x82148678 |
| `snapshot_t` | 0xD210 | 0xEE30 | **code** | 0x82100000 |
| `gameState_t` | 0x4E84 | 0x4E84 | **code** size | cgame trap 53, 0x82101DC0 |
| `glconfig_t` | 0x2C54 | 0x2C44 | **code** size and fields from 0x2C28 | cgame trap 52, 0x82101D94; `GLimp_Init` 0x8213D6E8 |
| `vmCvar_t` | 0x110 | 0x114 | **code** | `Cvar_Update` 0x82120AA8 |
| `trace_t` | 0x38 | 0x38 | **code** size, `fraction`, `entityNum` | `SV_Trace` 0x82138788 |
| `cplane_t` | 0x14 | 0x14 | **observed** | - |
| `polyVert_t` | 0x18 | 0x18 | **code** stride | `RE_AddPolyToScene` 0x82148090 |
| `markFragment_t` | 0x8 | 0x8 | **code** | `R_AddMarkFragments` 0x82161F78 |
| `orientation_t` | 0x30 | 0x30 | **code** | `LerpTag` 0x8215EDC0 |

**QAA's network structures are Quake III 1.32's, not Quake Live's.** FnQL
carries Quake Live's additions, so every structure that embeds
`entityState_t` or `playerState_t` differs and must be translated field by
field, not copied.

### trajectory_t (0x24)

`trType, trTime, trDuration, trBase[3], trDelta[3]` at 0x00/0x04/0x08/0x0C/
0x18 (**verified**). FnQL adds `float gravity` at 0x24.

### entityState_t (0xD0)

Exactly Quake III's: the 51-entry netfield table (0x827EF130) covers every
field except `number` (0x00). `pos` 0x0C, `apos` 0x30, `time` 0x54 ...
`generic1` 0xCC; field order and bit widths match Quake III (`eFlags` 19 bits,
`powerups` 16, `event` 10). **verified**.

Against FnQL: no `health`, `armor`, `location`, `jumpTime`, `doubleJumped`,
and trajectories are 4 bytes shorter, so every field from `apos` on is at a
lower offset (FnQL `time` 0x5C, QAA 0x54; `weapon` 0xD0 vs 0xC0).

### playerState_t (0x1D8)

Quake III 1.32's layout plus one int:

- 0x00..0x84 as Quake III (**verified**, netfield table 0x827EF3B0).
- 0x88 `externalEventTime` (**observed**: unnetworked, Quake III's position).
- 0x8C `clientNum`, 0x90 `weapon`, 0x94 `weaponstate`, 0x98 `viewangles`,
  0xA4 `viewheight`, 0xA8..0xB4 damage fields (**verified**).
- 0xB8 `stats[16]`, 0xF8 `persistant[16]`, 0x138 `powerups[16]`, 0x178
  `ammo[16]` (**code**, `MSG_WriteDeltaPlayerstate` 0x821278A0).
- 0x1B8 `generic1`, 0x1BC `loopSound`, 0x1C0 `jumppad_ent` (**verified**).
- 0x1C4 `ping` (**observed**), 0x1C8 `pmove_framecount` (**code**, qagame
  0x880335E8, masked with 63), 0x1CC `jumppad_frame` (**code**, qagame
  0x8802F3C0), 0x1D0 `entityEventSequence` (**code**, qagame 0x8802F744,
  cgame 0x8601097C).
- 0x1D4: **a QAA-only int** (**code**), not networked; provisional name
  `qaa_weaponsDisabled`. `ClientThink` (qagame 0x880355CC) sets it to 1 when
  `g_gametype` is 1 (tournament) and `level.warmupTime` is non-zero, else 0,
  before every `Pmove`; the pmove weapon routine (0x88032A64) returns at once
  while it is set, so nobody can fire during duel warmup. The cgame sets it
  the same way for prediction (0x8602EED0).

Against FnQL (0x1F8): FnQL moves `externalEventTime` to the end, inserts
`location` 0x8C, `weaponPrimary` 0x94, `fov` 0x9C, and appends `jumpTime`,
`doubleJumped`, `crouchTime`, `crouchSlideTime`, the mirrored move bytes and
`armorTier`. Every field from `clientNum` on is at a different offset.

### usercmd_t (0x18)

Quake III's: `serverTime` 0x00, `angles[3]` 0x04, `buttons` 0x10, `weapon`
0x14 (byte), `forwardmove` 0x15, `rightmove` 0x16, `upmove` 0x17 (signed
bytes). **code**. FnQL adds `weaponPrimary` 0x15 and `fov` 0x16 (0x1C total).

### sharedEntity_t / entityShared_t (0x204 / 0x134)

`sharedEntity_t` = `s` (`entityState_t`, 0x00) + `r` (`entityShared_t`,
0xD0). As in Quake Live and FnQL, `entityShared_t` starts with a block the
size of an `entityState_t` (**observed**; contents not checked). Then, all
**code**:

| Field | r offset | gentity offset | Source |
|---|---|---|---|
| linked | 0xD0 | 0x1A0 | `SV_LinkEntity` 0x82137DF8, `SV_UnlinkEntity` 0x82137D10 |
| linkcount | 0xD4 | 0x1A4 | 0x82137DF8 |
| svFlags | 0xD8 | 0x1A8 | 0x82136F78 (snapshot loop), 0x8212E8B8 (`SVF_BOT` 8) |
| singleClient | 0xDC | 0x1AC | 0x82136F78 |
| bmodel | 0xE0 | 0x1B0 | 0x82137DF8 |
| mins / maxs | 0xE4 / 0xF0 | 0x1B4 / 0x1C0 | 0x82137DF8 |
| contents | 0xFC | 0x1CC | 0x82137DF8 |
| absmin / absmax | 0x100 / 0x10C | 0x1D0 / 0x1DC | 0x82137DF8 |
| currentOrigin / currentAngles | 0x118 / 0x124 | 0x1E8 / 0x1F4 | 0x82137DF8 |
| ownerNum | 0x130 | 0x200 | `SV_ClipMoveToEntities` 0x821385A8 |

Field order matches FnQL; only the embedded `entityState_t` size differs
(FnQL `r` at 0xEC, total 0x23C).

**`G_LOCATE_GAME_DATA`** (game trap 15, **code**): the engine stores
`gEnts` 0x82E0FCC8, `sizeofGEntity` 0x82E0FCCC, `numGEntities` 0x82E0FCD0,
`clients` 0x82E0FCD4, `sizeofGameClient` 0x82E0FCD8. qagame (0x88051758)
passes `g_entities` (0x880DBEA0), `level.num_entities`, **`sizeof(gentity_t)`
= 0x348**, `level.clients` and **`sizeof(gclient_t)` = 0x360**. Each
`gclient_t` starts with its `playerState_t`, as in Quake III (**observed**).

### refEntity_t (0xC0)

Size **code**: `RE_AddRefEntityToScene` (0x821484E0) copies 0xC0 bytes into a
0x100-byte renderer slot; the cgame clears 0xC0 bytes before building one (41
sites). Fields **observed** from the cgame's stores:

- 0x00 `reType` (0..7, checked by the renderer), 0x04 `renderfx`, 0x08
  `hModel`, 0x0C `lightingOrigin[3]`, 0x18 `shadowPlane`, as Quake III.
- 0x1C..0x4F: a QAA-only 52-byte block, unused in practice (**observed**):
  none of the 27 cgame functions that submit a refEntity stores into it (the
  0xC0 memset clears it), and neither `R_AddEntitySurfaces` (0x8214AF08) nor
  any load through `tr.currentEntity` (0x82D56138) reads it. The bridge can
  pass zeros.
- 0x50 `axis[3][3]`, 0x74 `nonNormalizedAxes`, 0x78 `origin`, 0x84 `frame`,
  0x88 `oldorigin`, 0x94 `oldframe`, 0x98 `backlerp`, 0x9C `skinNum`, 0xA0
  `customSkin`, 0xA4 `customShader`, 0xA8 `shaderRGBA[4]`, 0xAC
  `shaderTexCoord[2]`, 0xB4 `shaderTime`, 0xB8 `radius`, 0xBC `rotation`:
  Quake III's order, each 0x34 later than in Quake III and FnQL (0x8C).

### refdef_t (0x170)

Identical to Quake III and FnQL: `x, y, width, height` 0x00..0x0C, `fov_x/y`
0x10/0x14, `vieworg` 0x18, `viewaxis` 0x24, `time` 0x48, `rdflags` 0x4C,
`areamask[32]` 0x50, `text[8][32]` 0x70. **code** `RE_RenderScene`
0x82148678.

### snapshot_t (0xD210)

`snapFlags` 0x00, `ping` 0x04, `serverTime` 0x08, `areamask[32]` 0x0C, `ps`
0x2C (0x1D8 bytes), `numEntities` 0x204 (clamped to 256), `entities[256]`
0x208 (stride 0xD0), `numServerCommands` 0xD208, `serverCommandSequence`
0xD20C. **code** `CL_GetSnapshot` 0x82100000. FnQL's is 0xEE30.

### gameState_t (0x4E84)

As Quake III and FnQL: `stringOffsets[1024]`, `stringData[16000]`,
`dataCount`. Size **code**, fields **observed**.

### glconfig_t (0x2C54)

Quake III's layout up to `isFullscreen` (0x2C38); the rest is QAA's. All
fields from 0x2C28 on are **code** from `GLimp_Init` (0x8213D6E8), which fills
them from the Xbox video mode (`XVIDEO_MODE` at device +0x1E3C):

| Offset | QAA | Quake III / FnQL |
|---|---|---|
| 0x2C28 / 0x2C2C | `vidWidth` / `vidHeight` (render size) | same |
| 0x2C30 | `windowAspect`, one of two constants picked by widescreen | same |
| 0x2C34 | `displayFrequency` | same |
| 0x2C38 | `isFullscreen` (always 1) | same |
| 0x2C3C | **`fIsWideScreen`** | `stereoEnabled` |
| 0x2C40 | **float `(float)(vidHeight / vidWidth)`**, integer division | `smpActive` |
| 0x2C44 | **int, always 0** | (end, 0x2C44) |
| 0x2C48 | **`smpActive`** (`r_smp`; "Using dual processor acceleration") | - |
| 0x2C4C | **`fIsInterlaced`** | - |
| 0x2C50 | **`dwDisplayWidth`** (physical output width) | - |

`hardwareType` 0x2C18 is read by `CG_Init` (0x86025730); the strings and the
other capability fields are **observed** at Quake III positions. The cgame
reads `vidWidth`/`vidHeight`; the DrawStretchPic trap (0x82101C9C) reads the
engine's own `cls.glconfig.vidHeight` (0x82F88260) to flip y.

### vmCvar_t (0x110)

`handle` 0x00, `modificationCount` 0x04, `value` 0x08, `integer` 0x0C,
`string[256]` 0x10. **code** `Cvar_Update` 0x82120AA8. FnQL inserts `flags`
at 0x10 (0x114).

### trace_t (0x38), cplane_t (0x14), polyVert_t (0x18), markFragment_t (0x8), orientation_t (0x30)

As Quake III and FnQL. `trace_t` size, `fraction` (0x08) and `entityNum`
(0x34) **code** (`SV_Trace`); `polyVert_t` stride and `modulate` (0x14)
**code** (`RE_AddPolyToScene`); `markFragment_t` `firstPoint` 0x00 and
`numPoints` 0x04 **code** (`R_AddMarkFragments` 0x82161F78, under
`MarkFragments` re+0x70); `orientation_t` `origin` 0x00 and `axis` 0x0C
**code** (`LerpTag` re+0x74, 0x8215EDC0, whose failure path writes both; its
success path is VMX128 code that Ghidra cannot decode). `cplane_t` and the rest
of `trace_t` **observed**.

### Botlib structures

The botlib traps (200..581) pass Quake III 1.32's botlib structures, and
FnQL's botlib uses the same layouts, except for the two entity structures:

| Struct | QAA | FnQL | Evidence | Source |
|---|---|---|---|---|
| `bot_entitystate_t` | 0xCC | 0xD0 | **code** | qagame 0x880278E8 (clears 0xCC bytes and fills them); `AAS_UpdateEntity` 0x821BEB30 |
| `aas_entityinfo_t` | 0xE8 | 0xEC | **code** | `AAS_EntityInfo` 0x821BEEE8 (copies 0xE8 bytes) |

Both are Quake III's (0x70 and 0x8C) followed by Quake Live's extension
(`nextthink`, `itemQuantity`, `itemType`, `itemTag`, `health`, `maxHealth`,
`ammoFull[16]`) and then `carriesFlag`, at 0xC8 and 0xE4: Quake Live's
`trainingClear` is not there. qagame sets `carriesFlag` when the client holds
`PW_REDFLAG` or `PW_BLUEFLAG` (gclient 0x154, 0x158), and `AAS_UpdateEntity`
copies state 0xC8 to info 0xE4. qagame passes the state through the engine
table (entry 0x24), not trap 207.

The other structures are taken to be Quake III's, which the trap cases'
argument conversions agree with; their layouts are not checked against QAA
code: `aas_areainfo_t` 0x34,
`aas_clientmove_t` 0x54, `aas_predictroute_t` 0x24, `aas_altroutegoal_t`
0x18 (three `unsigned short`s at 0x10), `bot_input_t` 0x28, `bot_goal_t`
0x38, `bot_moveresult_t` 0x34, `bot_initmove_t` 0x44, `weaponinfo_t` 0x228,
`bot_match_t` 0x148, `pc_token_t` 0x410, and the inventory as `int[256]`.

**QAA's botlib has no chat AI** (**code**). The AI half of the export table,
filled at 0x821A5020, points every chat-state call at a stub: `li r3, 0;
blr` (0x8260F748) for `BotAllocChatState`, `BotNextConsoleMessage`,
`BotNumConsoleMessages`, `BotNumInitialChats`, `BotReplyChat`,
`BotChatLength` and `BotLoadChatFile`, and `blr` (0x826F9BC8) for
`BotFreeChatState`, `BotQueueConsoleMessage`, `BotRemoveConsoleMessage`,
`BotInitialChat`, `BotEnterChat`, `BotGetChatMessage`, `BotSetChatGender`
and `BotSetChatName`. `StringContains`, `BotFindMatch`, `BotMatchVariable`,
`UnifyWhiteSpaces` and `BotReplaceSynonyms` are real, but default.xex holds
none of the files Quake III's `BotSetupChatAI` loads (`syn.c`, `rnd.c`,
`match.c`, `rchat.c`), and pak0 has none of them (**observed**, among the
named entries). qagame still calls `BotEnterChat` and `BotGetChatMessage`
with chat state 0.

## 7. Limits

Full table in `limits.tsv`.

| Name | QAA | FnQL | Evidence |
|---|---|---|---|
| MAX_GENTITIES | 1024 | 1024 | **code** 0x82137DF8; qagame memset 0xD2000 = 1024 × 0x348 |
| ENTITYNUM_WORLD / NONE | 1022 / 1023 | same | **code** 0x82138788 |
| **MAX_CLIENTS** | **16** | 64 | **code** `sv_maxclients` clamped to 16 (0x82133D60); qagame clients array 16 × 0x360 (0x88042BD8) |
| MAX_CONFIGSTRINGS | 1024 | 1024 | **code** 0x821315A8 |
| MAX_GAMESTATE_CHARS | 16000 | 16000 | **code** 0x82101DC0 |
| MAX_ENTITIES_IN_SNAPSHOT | 256 | 256 | **code** 0x82100000 |
| PACKET_BACKUP | 32 | 32 | **code** 0x82100000 |
| **MAX_PARSE_ENTITIES** | **2048** | 8192 | **code** 0x82100000 |
| CMD_BACKUP | 64 | 64 | **code** 0x82101E64 |
| MAX_STATS / PERSISTANT / POWERUPS / WEAPONS | 16 | 16 | **code** 0x821278A0 |
| MAX_PS_EVENTS | 2 | 2 | **verified** netfields |
| MAX_MAP_AREA_BYTES | 32 | 32 | **code** 0x82100000 |
| MAX_QPATH | 64 | 64 | **code** 0x82156A58 |
| MAX_STRING_CHARS | 1024 | 1024 | **code** 0x82126A34 |
| MAX_INFO_STRING / BIG_INFO_STRING | 1024 / 8192 | same | **code** 0x8212B394 / 0x8212B5BC |
| MAX_CVAR_VALUE_STRING | 256 | 256 | **code** 0x82120AA8 |
| refentities per scene | 1022 | - | **code** 0x821484E0 |
| `client_t` stride (engine-internal) | 0x2CB78 | - | **code** 0x82130438 |

## 8. What the bridge has to do

In short, from the facts above:

1. Call `dllEntry` (ordinal 2) with a guest-callable syscall entry that packs
   r3..r10 plus stack slots into `args[16]`, as `VM_DllSyscall` does.
2. Call vmMain 9 (game) / 12 (cgame) with guest tables whose entries resolve
   to host bridge functions taking direct-call arguments, before `GAME_INIT`
   / `CG_INIT`.
3. Implement the traps by QAA's numbers, not FnQL's: the game table is
   Quake III's with an insertion at 30 and `G_FS_SEEK` at 73; most cgame
   traps are offset by 1, 2 or 3 from FnQL's numbers.
4. Return floats as bits from game traps but truncated from cgame math traps.
5. Translate `entityState_t`, `playerState_t`, `usercmd_t`, `trajectory_t`,
   `refEntity_t`, `vmCvar_t` and `glconfig_t` field by field (byte-swapping
   everything); `refdef_t`, `trace_t`, `gameState_t`, `polyVert_t` share
   FnQL's layout and only need swapping.
6. Keep `MAX_CLIENTS` at 16.
7. Implement game trap 72 and cgame trap 141 as a local mailbox (section 5);
   the other Live, achievement and rumble traps can be PC stubs or PC
   equivalents, documented as deviations.

## 9. How this was recovered

- `default.xex` and the modules were decoded to PE images (`tools/xex.py`)
  and read with Ghidra 12.1.3 headless and a small Python/Capstone tracer.
- Ghidra's PE loader rejects machine 0x1F2, so the images are imported raw
  (`BinaryLoader`, base address, `PowerPC:BE:64:64-32addr`). The scripts in
  [`tools/ghidra/`](../tools/ghidra) then split sections, seed functions from
  `.pdata`, add every direct call target (most leaf functions have no
  `.pdata`), and commit decompiler-recovered signatures:
  `XexPeSetup.java`, `SeedCallTargets.java`, `CommitParams.java`,
  `DecompileFunctions.java`. They work for any decoded Xbox 360 image.
- Ghidra's PowerPC language does not decode VMX128, so disassembly stops at
  the first VMX128 instruction; none of the functions read here use it. The
  `.pdata` lengths still bound every function.
- Decompiler output stays under `ref/` and is never committed; this file and
  the TSVs hold facts only.
