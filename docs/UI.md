# The Scaleform front end (M6)

Quake Arena Arcade has no `ui` module. Its menus, HUD and scoreboards are
Scaleform GFx 2.x movies (`ui/*.gfx` in pak0.bpk) played by the **SFUI** layer
of `default.xex`. FnQAA recompiles SFUI and the GFx player from the user's
own `default.xex` into a guest module, `qaa_ui`, and hosts it in the client.
The movies run their own ActionScript, and SFUI runs its own C++. The engine
functions SFUI calls are implemented over FnQL. What SFUI's renderer records
is replayed by a software rasteriser into a canvas, which the client draws
over the frame.

Facts are marked as in the other documents: **verified** (checked against
retail behaviour or data), **code** (read from the disassembly) or
**observed** (seen in headless runs).

## Pieces

| Piece | Where |
|---|---|
| Recompiled SFUI + GFx | `qaa_ui` guest module: `tools/recomp/build.py --modules ui`, configured by `data/recomp/ui.json` ([RECOMP.md](RECOMP.md#subset-modules)) |
| Host side | `code/client/cl_qaa_ui.cpp` (`QAA_UI_*`, C linkage, `cl_qaa_ui.h`) |
| Rasteriser and image decoders | `code/client/cl_qaa_ui_raster.cpp` (no engine dependencies; `tests/qaa_ui_raster_tests.cpp`) |
| Guest runtime additions | `code/guest`: host hooks for non-recompiled functions and unimplemented imports, register-level guest calls, stepping a guest thread |
| Client wiring | `cl_main.cpp` (start-up in `CL_InitUI`, frame), `cl_scrn.cpp` (draw), `cl_keys.cpp` (keys), `cl_cgame.cpp` (load and first-snapshot events), `cl_qaa_cgame.inc` (cgame traps 116-140), `sv_qaa_game.inc` (game traps 48, 49, 65, 69) |

## The module

**code** SFUI is 0x8218DF98..0x821A46F0 of `default.xex`: start-up
(`SFUI_Init` 0x8218E048), the update thread (0x82196458), render
(0x8218E5A8), keys (0x8218ED18), the event and invoke queues, the
ExternalInterface `FgApi` (58 functions, table 0x827EB9F0) and fscommands
(0x821915E8). It also holds Quake III's `ui_main` data layer
(0x8219A000..0x821A46F0), which the movies' feeders use through ui syscalls.
The renderer is 0x82166900..0x821687F0. GFx, zlib, the CRT and the XAM/XGraphics
helpers are library ranges.

`tools/recomp/select_funcs.py` takes the closure of those roots through
calls, branches, code-pointer references and data tables. The library
ranges are followed and the rest of the engine is not. About 6,350 of the
image's 17,558 functions are kept. Every engine function the kept code
calls becomes a **hook**: a stub that calls the host. The host implements
the ones SFUI needs (below) and returns 0 from the rest, logging each
once (`unhandled engine call`). An engine function that only computes from
its arguments and the image's own tables is recompiled instead (the
`recompile` list in `ui.json`): the feeders' column count (0x821894F0,
**code**: 3 for the model heads, 6 for the maps, 1 for most) behind
`FgApi.getFeederColumnCount`. As a hook it returned 0, so every feeder list
had no columns and never asked for its rows' text (**observed**: the model
selector's grid stayed empty and the map selector's rows kept their
authoring text, "WWWW").

The module is loaded at 0x82000000 (the image base) into the same guest as
qagame (0x88000000) and cgame (0x86000000). The three share one address
space, as on the console, so the cgame's and qagame's UI traps pass guest
pointers straight through. The CRT is started first (heap, `_mtinit`, the
initializers). Then `SFUI_Init` runs on the client's thread.

## Engine functions SFUI calls

| Function | Address | Host |
|---|---|---|
| ui syscalls (`CL_UISystemCalls`) | 0x8210F988 | QAA's numbering: Quake III's up to 0x1D, +1 from 0x1F (0x1F `S_RegisterSound` ... 0x2C `GETGLCONFIG`, 0x33 `CVAR_REGISTER`, 0x34 `CVAR_UPDATE`); floats return as bits |
| `Cvar_FindVar/Get/Set2/SetValue` | 0x8211F548, 6C8, 908, BE8 | FnQL cvars, mirrored into guest `cvar_t` (Quake III layout, 0x2C bytes) |
| `Cbuf_AddText`, `Cmd_AddCommand` | 0x82119708, 0x8211A158 | FnQL; guest commands are called back |
| `Z_*`, `Hunk_*Temp` | 0x8211B258.., 0x8211C180.. | the guest CRT heap (malloc 0x821DDBA8) |
| `FS_*` | 0x82121B28.. | FnQL's file system (the package and pak0.bpk) |
| `S_StartLocalSound`, `S_RegisterSound` | 0x82173418, 0x82173DC0 | FnQL sound |
| background track (intro, loop) | 0x82173470 | `S_StartBackgroundTrack`: the menus' music (`playmusic`) |
| `Sys_Milliseconds`, language, country | 0x82185850, 0x82184DA8, 0x82184E58 | "en", "us" |
| D3D texture create/lock/unlock/release/desc | 0x82204038, 0x82203EA0, 0x822023E8, 0x822021E0, 0x82204030 | host textures behind a guest 'QTEX' block |
| `D3DXCreateTextureFromFileInMemoryEx` | 0x8221D000 | DDS (DXT1/2/3/5, 32-bit ARGB) and TGA, decoded to RGBA; fills `D3DXIMAGE_INFO` |
| `R_GetCommandBuffer` | 0x821408A8 | unused by the replay; returns a scratch block |
| SFUI-Backend constructor | 0x821687F0 | a host object (below) |
| licence, profile, storage, config | see [PC deviations](#pc-deviations) | |

**code** The menus' lists come from `ui_main`: `FgApi.getFeederCount`,
`getFeederText`, `getFeederIndexByCvar` and `getMapValueForKey` read its
`uiInfo` (0x82D04DE0: the arenas from `scripts/arenas.txt`, the game types,
the characters) and the cvars it registers at start-up (`UI_RegisterCvars`
0x821A2680, 114 of them: `ui_mapIndex`, `ui_currentMap`, `ui_gametype`,
`ui_modelIndex` with its default of 22...). Until `CVAR_REGISTER` was
handled none of them existed, the movies read their indices as "" and
asked for row NaN (**observed**). Feeders that are not `ui_main`'s (the
Live lists, e.g. 17) go to the engine through traps 0x6F and 0x70
(0x821897E8, 0x821895A0), which the host does not implement.

SFUI also reads engine globals in place. The host publishes them before
every update and every key event:

| Global | Address | Contents |
|---|---|---|
| `cls.state`, `cls.keyCatchers` | 0x82ED88C0, 0x82ED88C4 | merged both ways (below) |
| `cls.realtime` | 0x82ED89E8 | events are processed every 33 ms of it |
| glconfig | 0x82F85634 and the renderer's copy 0x82DC0480 | `vidWidth` +0x2C28, `vidHeight` +0x2C2C, widescreen +0x2C3C |
| profile records | 0x82A751A8, 0x498 bytes per controller | controller 0 signed in, gamertag from `name` |
| Live, profile and data-layer blocks | *0x82C01C04, *0x82A6EA58, *0x82C01C78 | zeroed blocks: no Live session |

**Key catchers.** On the console there is one `cls.keyCatchers`, which
`FgApi.disablePlayerInput` and `restorePlayerInput` set and clear. Here the
engine's copy and the guest's are merged, and whichever changed since the
last merge wins, the guest first. The update thread can change its copy
between two merges, and copying the engine's value over it would undo a
`restorePlayerInput` (**observed**: the in-game menu would not reopen).
Applying a change through `Key_SetCatcher` releases the keys held at that
moment through `CL_KeyEvent`, and the front end takes those releases, so the
merge does not run again while it applies one (**observed**: run again, it
re-applied the same change without end; Escape held down while the in-game
menu opened overflowed the stack).

## Frames, input and events

**code** The update thread (0x82196458) waits for the engine's 60 Hz frame
event (sfui+0x361ED8). Each wake advances every movie by 1/60 s and runs
the queued invokes, input and events. `QAA_UI_Frame` (before
`SCR_UpdateScreen`) sets the event and runs the thread until it waits
again. That is one iteration per 1/60 s of real time, at most four per
frame. PC deviation: retail overlaps the update with the render thread;
here the two never run at once, as FnQL is single-threaded.

**code** SFUI's render (0x8218E5A8, through 0x82196E48) also sets the
frame event as it ends (0x8219706C), letting the update thread work out
the next frame while the render thread goes on. Left alone, that iteration
ran beside the engine: with `+map` or `+devmap` on the command line it
read images while `SV_SpawnServer` restarted the file system, and the
thread died. So while `QAA_UI_Draw` runs the render, the frame event's
slot holds a private event (the update thread is parked on the real one
and does not read the slot then); if the render set it, the iteration it
lets go runs as one step of the update thread before the draw goes on.

Keys follow retail `CL_KeyEvent` (0x82106D20). While SFUI holds
`KEYCATCH_UI` it gets every key, as (key, down, controller 0). Otherwise
Escape (or Start) in a game sets `_global.uiValues[0] = 1`, which opens the
in-game menu, unless `cl_paused` is set. Every other key goes to the
bindings. PC deviations: the console keeps its keys, and the keyboard
drives the menus as the pad does (SFUI translates through its table at
0x827EBBC8).

**The mouse.** PC deviation: the Xbox has none. **code** The movies'
mouse handlers do not select or activate anything: QAA's `FgUIComponent`
uses `onRollOver`, `onPress`, `onRollOut` and `onRelease` only to show its
`_over`, `_down` and `_up` frames, and SFUI registers no mouse with GFx
(the movie's mouse count, +0x9B8, is 0, so GFx drops mouse events). What
selects and activates is the scene (`FgScene`): `setSelection(i)` over its
`selectionArray`, and its `onKeyDown`, which takes A to
`currentSelection.activate()`, B to `goBack()` and the rest to the
selection's own `onKeyDown`. So the pointer drives the scenes as the pad
does, through the movie's `GetVariable` (vtable +0x30) and `Invoke`
(+0x48):

- Over a selectable component it selects the component, by `hitTest` on
  bounding boxes in the stage's pixels (the viewport mapped as GFx maps its
  mouse, movie +0x70..+0x7C). Neighbouring boxes overlap (a menu item's
  holds its highlight bar), so of several the one the pointer is most
  central in wins, measured in each box's own size. Inside a grid
  (`FgGridSelector`: `grid[]`, `setGridSelection`) or a list (`FgListBox`:
  `rowList[]`, `setSelection`) the square or row is selected too. Off
  every component the selection stays.
- The left button presses A on what it is over. On a slider (`FgSlider`)
  the track (`line`) sets the value at the pointer, snapped to its step,
  and the value follows while the button is held; left or right of the
  track (the arrows) presses left or right. On a rotator (`FgRotator`)
  the button presses left or right of the value's middle. On a slider's or
  rotator's label it only selects. On a scene whose one component is a
  button (the title's "Press A to start") a click anywhere is on it; on a
  scene with none (the start-up movies) it presses A. Beside every
  component of a scene, a click does nothing.
- The right button (and the fourth) presses B, as Escape; the wheel
  presses up and down, so it moves the selection and scrolls a list.

The current scene is `_global.sceneManager.movies[movies.length - 1].scene`:
`currentScene` is an `addProperty` getter, which GFx's `GetVariable` does
not run. Pointer events are queued and run before each step of the update
thread, while it is parked; the keys they become then go through SFUI's key
queue after the selection changes. The front end owns the pointer
(`QAA_UI_WantsPointer`) in its menus and wherever it shows outside a game
(the start-up movies, the title, loading), where SFUI holds no
`KEYCATCH_UI`: `Key_GetPointerCatcher` adds `KEYCATCH_UI` for the SDL,
Win32 and X11 layers, which then show the system pointer (confined to a
fullscreen window) and report absolute positions. A button the front end
took is released there, and one the game took is released in the game.

**The console.** The Xbox has none. Here it opens and closes over the
front end at any time. Quake III's rule that a disconnected client shows
the console full screen, and cannot close it, applies only when the front
end is not running (`qaa_ui 0`, or no recompiled ui module): SFUI does not
hold `KEYCATCH_UI` during the start-up movies or at the title (**observed**),
so under that rule the console covered the menus and kept every key. While
the front end runs, a closed console takes no keys and no typing.

**Region.** XGetLocale's country code is the console's region. The startup
movie (`ui/startup.gfx`) shows the ESRB rating screen (`icons/rating_esrb`)
and the US legal text for `us`, and the rest-of-world legal text with no
rating elsewhere (**observed**: `rating_blank`, which the package does not
hold). PC deviation: the region is the operating system's (Windows' home
location, else the territory in `LC_ALL`, `LC_MESSAGES` or `LANG`), or
`qaa_country` when set; `us` when none says. The language is always `en`.

The engine's own calls into SFUI are made where retail makes them.
`ClientLoadComplete` is sent at the end of `CL_InitCGame`. On the first
snapshot the HUD scene is invoked (`_global.sceneManager.gotoScene`, menu
id 4). The cgame's traps 116-140 are handled as retail's
`CL_CgameSystemCalls` (0x821008B0): events with their format strings,
`_global.hudData`, `_global.uiValues`, the scoreboard blocks. The game's
48 (`gotoStackPath`), 49 (in-game menu), 65 (`_global.courseResults`) and
69 (cached host name and scores) are handled as `G_SystemCalls`
(0x82130438) does; the server hands them to the client through
`QAA_SV_GameUiTrap`, so a dedicated server keeps retail's return values.

## Cvars, binds and commands the menus use

**code** The movies reach the engine through `FgApi.setCvar` (an
`FSCommand:setCvar`), `getCvarString`, `getCvarValue` and
`getFeederIndexByCvar`, and run commands with `FSCommand:qcmd`. Most cvar
names sit in the movies' code; the settings screens' components carry
theirs as component parameters (`cvar`, with the value list in
`dataValue`). Every name the 48 movies use was checked against a headless
client's `cvarlist` (**observed**). Each one retail's engine registers now
exists with retail's default:

- `CL_Init` (0x8210A860): `xSensitivity` 5, `ySensitivity` 2.5,
  `cl_southpaw` 0, `cl_vibration` 1, `cl_mouseAccelLowScale` 0.5,
  `cl_mouseAccelHighScale` 1.25, the leaderboard filters, and the Custom
  control scheme's 14 `keybind_btn_<action>_custom` (retail pad key names).
  `model` and `headmodel` start as `james` (ui_modelIndex 22),
  `cg_autoswitch` as 1, `color1` and `color2` as 4 and 5, without Quake
  Live's [1, 26] bounds (the rail colours are 0 to 6).
- `GLimp_Init` (0x8213D6E8): `r_HoldFrame` (every movie sets it; nothing
  reads it), `r_widescreen` and `r_videowidth`, which the HUD reads to place
  its obituaries. `r_widescreen` is the front end's glconfig flag (16:9 or
  wider).
- `S_Init` (0x82174978): `s_musicVolume` 0.6.
- The Live start-up and lobby: `xb_liveConnected`, `xb_isInParty`,
  `xb_primaryLiveConnected`, `xb_signinRunning` (0, ROM) and
  `ui_inServerBrowser`.

The rest (`ui_pleasewaitCaption`, `keybind_btn_<action>`,
`cg_forcemodelTemp` and others) are created by the movies' own `setCvar`,
as on the console. `ui_ControlScheme` picks a file in `controlschemes/`
(default, basic, dmertz, sniper), which the Controls screen runs with
`exec controlschemes\<name>.cfg`, so its keys are renamed as `default.cfg`'s
are (`qaa_binds.h`).

The Custom Controls screen (`customcontrols.gfx`) binds the pad itself:
`bind <key> "<action>"` and `unbind <key>` with retail key names (JOY1..JOY10,
MOUSE1 and MOUSE2 for the triggers, the arrows for the D-pad), and Clear All
sends `unbindall;`, then binds Back and the right trigger again. PC
deviation: the front end's command text is rewritten as it reaches the
command buffer (`H_Cbuf_AddText`, `QAA_RewriteFrontEndCommands`): the keys
take the pad's PC names, and `unbindall` unbinds the pad's buttons only.
Sent unchanged, the scheme's binds that opening the screen sends would bind
the PC's mouse buttons and arrow keys, and Clear All would remove every
binding, Escape and the console key included. **observed**: after opening
the screen, binding Next Weapon to X and Clear All, `bindlist` shows the
pad's binds on `PAD0_*` and the keyboard's and mouse's unchanged.

The settings screens' cvars act as on the console:

- **The look stick.** Retail reads both sticks without binds (its pad
  input, 0x82182A08): the move stick, and the look stick, sent as a mouse
  motion that `CL_MouseMove` (0x82104900) scales by `xSensitivity` and
  `ySensitivity` and turns by `m_yaw` and `m_pitch` (Invert Look sets it to
  -0.022). `cl_southpaw` swaps the sticks. FnQAA's SDL layer sends the look
  stick's position as joystick axes `AXIS_YAW` and `AXIS_PITCH` and
  `CL_PadLookMove` turns it with the same dead zone, counts and scales
  (`fnql::input::QaaPadLook`), the 60 Hz frames of the Xbox spread over the
  PC's frame time. PC deviation: the move stick keeps its binds
  (`PAD0_LEFTSTICK_*`, whichever stick moves), and the look stick has none.
- **Vibration.** The cgame's trap 113 (user, left %, left ms, right %, right
  ms) vibrates the pad (SDL's rumble) while `cl_vibration` and
  `sys_titleFocused` are set, as retail's 0x82183C20 does with the
  profile's setting; trap 112 stops it.
- Sound, music and brightness are `s_volume`, `s_musicvolume` and
  `r_gamma`; Zoom is the cgame's `cg_zoomToggle`.

`ready`, `startgame` and `qnet_disconnect` (the lobby's, from `qcmd`) are
retail's session layer and have no PC command yet (Host Match's Start Match
starts the match without the lobby, [PC deviations](#pc-deviations))
([NETWORK.md](NETWORK.md)). Retail has no `pause` command either: the
schemes bind Start to it, and the engine takes Start for the in-game menu
first.

## The renderer back end

**code** GFx 2.x's `GRenderer` (SFUI's, vtable 0x820CB800) records commands
into double-buffered 4 MB lists. The recorder stays the retail one. The
object that turns the lists into Xenos draws, QAA's "SFUI-Backend" (ctor
0x821687F0, vtable 0x820CE37C), is replaced by a host object. Its device
textures (vtable 0x820CE390) are host textures. `QAA_UI_Draw` calls SFUI's
render, then replays the list:

| Id | Command | Id | Command |
|---|---|---|---|
| 1 | BeginDisplay (viewport, clip, background) | 12 | FillBitmap (texture, matrix, wrap, sample) |
| 2 | EndDisplay | 13 | FillGouraud (type, up to three textures) |
| 3 | DrawBitmaps (glyphs) | 14/15 | Push/Pop blend mode |
| 4/5 | Vertex/Index data | 16/17 | Matrix / user matrix |
| 6 | DrawTriangles | 18 | Colour transform |
| 7 | DrawLineStrip | 19/20/21 | Begin/End/Disable mask |
| 8-11 | Fill/line disable and colour | 22 | Frame end |

The fill rules come from the back end's own pixel shaders (HLSL source in
the image at 0x820CB900..0x820CE300). Output is premultiplied. Gouraud fills
take `lerp(colour, tex0, factor.b)`, or `lerp(tex1, tex0, factor.b)` with two
textures. Edge anti-aliasing multiplies alpha by `factor.a`. Glyphs take
only the texture's alpha. A texture fill's matrix rows are divided by the
texture's size (0x82169490). Colours differ by source:

- Shape vertex colours are 0xAABBGGRR, so R and B are swapped.
- Bitmap and fill colours are 0xAARRGGBB.
- Colour transforms add in 0..255.

Masks are stencil levels. Blend modes follow the shaders
(multiply/darken/lighten/screen mix the source toward white by its
coverage). `cl_qaa_ui_raster.cpp` draws all of this into a canvas the size
of the video mode (up to 4096 pixels a side, scaled to fit beyond that).
Replaying a frame only queues its triangles, resolved against the state
of the moment; the canvas then rasterises the queue in bands of 16 rows on
worker threads, every band running the triangles that reach it in order
over its own rows, and converts each band it drew to straight alpha. Rows
no triangle touches and that were already transparent are neither cleared
nor converted again, and only the ranges of rows that were written are
uploaded: in a game the HUD is a strip of the screen. The client presents
the canvas over the frame with `re.DrawWebUISurface` (a renderer surface
blit; there is no WebUI), passing those ranges. The renderers keep that
surface at the canvas size: their image loader scales anything over 2048
pixels down, so it is created at 1x1 and sized on first use.

Frames are skipped two ways. A client frame with no SFUI update shows the
last canvas again. Otherwise each band is hashed from the triangles that
reach it, as queued: positions, the fill and its planes, colour
transform, blend mode, mask and the versions of the textures sampled.
A band whose hash matches the last frame's keeps its pixels and is not
converted or uploaded: a frame that repeats draws nothing, and an
animation redraws only the bands it covers (in the menus, the selection's
glint: about 110 of 1440 rows). Only fields a fill uses enter its hash.
The command lists themselves change every frame where nothing visible
does (**observed**: a colour transform replaced before anything is drawn
with it, and stale data in a gouraud fill's unused texture slots), so
hashing them redrew the whole main menu, and the HUD after the in-game
menu, on every frame. A frame flushed in two parts (a texture changed
while triangles were queued) redraws every band of the next one.

Textures are host objects SFUI creates, changes and frees on its update
thread and on its own loader threads, which run alongside the engine. The
table is locked. Every change gives the texture a new version once its
texels are written, so the bands that sample it redraw. On the drawing
thread, a texture change first rasterises what is queued; a texture
freed on another thread is kept until the next draw starts.

Images: the movies' own bitmaps arrive as GImages (RGBA, RGB, A8, DXT1/3/5).
External images (`ui/*.dds`: the title and menu art, level shots, portraits,
button glyphs) come through `D3DXCreateTextureFromFileInMemoryEx`, as on the
console (**code**: SFUI's loader 0x82194CE8 tries `.dds`, then `.tga`, then
the bare name).

## PC deviations

| Retail | PC | Where |
|---|---|---|
| The licence (`XamContentGetLicenseMask` bit 0, cached at 0x82CD9D6C) decides full game or trial | always the full game: the user's own package | 0x821856C8; 0x821859B8 (may play); xam import 614 itself, which ui_main's model count (0x821A2FB8) asks |
| `CL_InitUI` (0x8218E490) sets `ui_ethernetActive` from `XNetGetEthernetLinkStatus`; System Link needs it | the link is FnQL's networking: 1 unless `net_enabled` is 0 | `QAA_UI_Init` |
| Sign-in through the Guide; profile records filled by XAM | controller 0 is signed in, gamertag = `name` | 0x821869D8, profile records |
| `XShowDeviceSelectorUI`, completed by the frame poll 0x8218C890 | one device (the home path), chosen at once; `DeviceSelectionComplete` is queued from the next frame | 0x8218CA70 |
| Profile settings in a save container (`startLoad` 0x8218BCE8, `startSave` 0x8218BDB0, completion `ConfigLoadComplete`/`ConfigSaveComplete`) | the settings are the engine's cvars and config; the container operations succeed with nothing to read or write | |
| Host Match's Start Match (`startServer`, 0x82192740) signals QNET's session thread, which hosts, opens the lobby and, once the host starts the match there, runs ui_main's server start (0x8219F5D8: `g_gametype`, the bots, `map`) ([NETWORK.md](NETWORK.md)) | no session layer yet (M8): the match starts at once, as the host's session does on reaching play. "Joining server" shows, then closes (`WaitEnd`) as the session's steps close it, and 0x8219F5D8 runs. No lobby, invites or session; others join the listen server with `connect` | `H_QnetJoinWait`, `PollHostStart` |
| `exitGame` flags the Live session (bit 3 at +0x1FC); the session manager (0x82176370) ends it: disconnect, `gotoStackPath(..., "mainmenu")`, client state reset (0x82177AB0, 0x821779E0, 0x821771F0) | the flag is polled; FnQL's `disconnect`, then the same stack path and resets. The course-results path variant is not taken | |
| `sys_titleFocused` from Guide focus notifications | follows the game window's focus; a windowless client is always focused | |
| A game can only start from the menus, after the title | a game can also start from the command line (`+map`, `+devmap`) while the start-up movies play. The first snapshot's HUD request then finds no scene manager, and the title would load over the game. When the front end reaches the title (`setGameState(2)`, 0x82C01C48) with a game running, the HUD is requested again, and the title's music is not started over the game's | `PollGameStartedBeforeTitle`, `H_S_StartBackgroundTrack` |
| Update and render overlap on two threads | one thread; the iteration the render lets go runs as a step | `QAA_UI_Frame`, `RenderMovies` |
| GPU replay | software rasteriser | |
| No mouse | the pointer selects, A on a click, B on the right button, the wheel as up and down; sliders take the pointer's place on the track ([the mouse](#frames-input-and-events)) | `RunPointerEvents`, `Key_GetPointerCatcher` |
| The pad is the only bindable device; the front end binds it under JOY1..JOY10, MOUSE1/2 and the arrows, and its `unbindall` clears it | the front end's commands take the pad's PC key names, and its `unbindall` clears the pad's buttons only ([the menus' binds](#cvars-binds-and-commands-the-menus-use)) | `H_Cbuf_AddText`, `QAA_RewriteFrontEndCommands` |
| `default.cfg` binds the pad only; there is no keyboard or mouse | after `default.cfg` the client runs FnQAA's `fnqaa.cfg` from `FnQAA-pkg.fnz` beside it (`pkg/baseq3/fnqaa.cfg`): Quake Live's keyboard and mouse binds and the move stick's. Saved configs run after both | `Cmd_Exec_f` |
| The move stick is read without binds | it drives `PAD0_LEFTSTICK_*` (whichever stick moves); the look stick is read without binds, as retail | `IN_GamepadMove`, `CL_PadLookMove` |

## Testing

Every run is headless (`com_headless 2`, no sound, no input grab).

- `qaa_uiTestFrames N`: run N client frames, then quit.
- `qaa_uiTestEvery K`: dump the canvas every K frames to
  `screenshots/qaa_ui_NNNN.tga`.
- `qaa_uiTestKeys "frame:KEY ..."`: scripted key presses (bind names,
  `MOUSE1` and `MWHEELDOWN` included); `+KEY` only presses, `-KEY` only
  releases (a drag).
- `qaa_uiTestPointer x y`: the pointer at renderer pixel (x, y), through
  the engine's absolute pointer path.
- `qaa_uiGet path`: a variable of the movie, as `GetVariable` resolves the
  path (for example `_global.sceneManager.movies.0.scene.selectionIndex`).
- `qaa_uiTestCmds "frame:command|..."`: console commands at given frames.
- `qaa_uiDump`: dump the canvas now.
- `qaa_uiTrace 1..6`: increasingly detailed tracing. 1 logs FgApi calls
  ("ui call:"), fscommands, file and cvar calls, key routing and key-catcher
  changes. Higher levels add traps, draws, recorder calls and per-command
  replay.
- `qaa_ui 0` (latched): run without the front end, console only.
- `qaa_uiProfile 1`: once a second, the front end's frame rate, how many
  frames drew some band and how many repeated the one on screen, and the
  average cost per frame (the pointer's events, update thread, SFUI
  render, recording the commands, rasterising, upload), with the
  triangles each redraw queued and the pixels, rows drawn and rows
  uploaded.
- `QAA_UI_THREADS=N` (environment): rasterise on N threads in all (1: on
  the calling thread only).
- A windowless client (`com_headless 2`) never changes the display mode,
  so it uses the windowed size: `r_windowedMode -1` with
  `r_windowedWidth`/`r_windowedHeight` tests other sizes (2560x1440 found
  the 2048-pixel surface limit). `screenshot` saves what the renderer
  presented, `qaa_uiDump` only the canvas.
- `qaa_country <cc>`: the region the start-up movies see (`us` shows the
  ESRB screen).

**observed** at 1600×1024, in order:

1. The legal screen, the id, Bethesda and Pi Studios logos, and the title
   with its art and A-button prompt (retail's order). The legal screen is
   the US one with the ESRB rating only for region `us`.
2. Enter: sign-in, storage, config load, then the main menu with its art.
3. Multiplayer (Player Match, Ranked Match, System Link).
4. `devmap q3dm6`: the HUD (armour, health, ammo with icon, frags, clock,
   pickups). `+scores` shows the FFA scoreboard with skill stars,
   portraits and medals.
5. Escape: the in-game menu, which pauses the game. Resume returns to the
   game, and the menu reopens.
6. Exit Game, then Yes in the confirmation: disconnect, then the Practice
   menu.
7. Help & Options, Settings, Player Settings, Select Model: the grid of
   the characters' portraits, 68 of them on two pages (`datasource._rowCount`
   68, 3 columns), with the default model (James) selected.
8. Multiplayer, System Link, Host Match, Change Arena/Gametype: the arenas
   of the game type by name, 32 for Free For All (as many as `uiInfo` holds
   with that type), and the selected one's level shot, suggested players
   and items.

Items 7 and 8 were checked at 2560×1440 (`qaa_uiGet
_global.sceneManager.movies.4.scene.selectionArray.0.datasource._rowCount`).

`tests/qaa_ui_raster_tests.cpp` covers the rasteriser and the decoders on
synthetic data. It needs no retail files.

## Known gaps

- Xbox Live: sessions, matchmaking, leaderboards and gamer cards answer
  "none". Achievements and gamer pictures are partial. Retail's session
  layer is described in [NETWORK.md](NETWORK.md).
- The map name in the scoreboard is "null" when a map is started from the
  console rather than from the menus.
- Performance: the rasteriser is a CPU path. Each row is walked only over
  the span a triangle covers, the common fills (colour, texture, gouraud,
  glyphs, normal blending) run four channels at once with SSE2, opaque
  point-sampled texels and opaque solid colours are copied as bytes, and
  gouraud fills whose factors are the same at every vertex are drawn as the
  texture or colour they reduce to. **observed** with `qaa_uiProfile` at
  2560×1440 on a 20-thread laptop (12 raster threads): the main menu
  redraws its glint in about 2-3 ms a frame at 63 frames a second (it
  redrew all 1440 rows every frame in about 28 ms, at about 30 frames a
  second); a full redraw of it takes about 9.5 ms, of the title about 6.5
  ms; frames that repeat cost about 0.1 ms; in a game the HUD redraws a
  few times a second, about 100 rows. Every frame of a run through the
  start-up movies, the title, the menus, a game and the in-game menu
  matched a full redraw of the same triangles byte for byte. In a game,
  SFUI's update thread itself (the HUD's ActionScript, recompiled guest
  code) takes about 1.5-3 ms a frame; retail overlapped it with rendering,
  FnQAA runs it with the engine waiting. A GPU replay would replace the
  rasteriser.
- The menus' `stopVibration` (0x82190B18) goes to the recompiled XAM
  helper (0x821F2468) and does not reach the pad; the cgame's rumble does
  (its pulses last 100 to 400 ms). XAM import 752 is unimplemented. The
  pad's look and vibration are tested on synthetic values only
  (`fnqaa_input_compat`); headless runs have no pad.
- The engine's own feeders (traps 0x6F and 0x70: the Live lists) answer
  nothing.
