# Changelog

This is the player-facing release-note queue for the next FnQAA release, kept
the way FnQL keeps its own (`scripts/changelog.py`, `scripts/manual_release.py`).

Add short user-facing entries under `Unreleased` as changes land, in the
category that fits. The release notes publish this queue as written:
`Highlights` entries lead, then every other entry under its category. Write
each entry as a complete sentence for players and server operators; indent
wrapped lines so they stay part of the entry. If `Highlights` is empty, the
first entry of each category is promoted. When a release needs editorial
control, add curated notes under [`releases/`](./releases/README.md). Highlights
made from the commit subjects are used only when this queue is empty. After a
published release, the release workflow resets `Unreleased` for the next
cycle (`python scripts/changelog.py clear-unreleased`).

## [Unreleased]

### Highlights
- Quake Arena Arcade runs natively on the PC from your own XBLA package: its
  game code and its Scaleform menus, HUD and scoreboards are recompiled from
  your copy on your machine, and local games with bots play on Windows and
  Linux.
- Setting up takes one step: put your copy of the game in the FnQAA folder
  (the XBLA zip, the zip extracted, your Xbox 360 Content folder or the
  package file) and run `setup.cmd` or `./setup.sh`. The engine then finds
  the game on its own, with no paths to type.

### Compatibility
- The start-up sequence follows your region as the console does: the
  legal notice without the ESRB rating outside the US, then the id,
  Bethesda and Pi Studios logos. Set `qaa_country us` for the US screens.
- The engine reads the game's data inside your package, in place, and never
  changes it; saved games and title updates stored beside the game are left
  alone.
- The keyboard and mouse start with Quake Live's default binds (WASD, mouse
  to fire and zoom, number keys for weapons, F to switch back to your last
  weapon), and the controller keeps Quake Arena Arcade's own layout,
  D-pad weapon shortcuts included. A config you saved earlier keeps the
  binds it holds.
- The menus work with the mouse. Pointing at an item selects it, as the
  D-pad would; clicking presses A on it, including a list's rows, a grid's
  squares and the title; clicking a slider's track sets its value, and
  dragging along it follows the pointer; clicking either side of an
  option's value steps it left or right. The right mouse button goes back,
  as B, and the wheel moves the selection. The pointer shows over the
  menus and the in-game menu, and the game keeps the mouse while you play.
- The controller works as on the console: the right stick looks around
  with the X and Y Sensitivity, Invert Look and Lefty settings of
  Help & Options, Controls, and the controller vibrates when you fire or
  take damage unless Vibration is off. The look stick no longer needs
  binds.
- Control Scheme binds the controller's buttons without touching the
  keyboard or mouse: choosing a button no longer rebinds the mouse
  buttons or arrow keys, and Clear All clears only the controller.

### Rendering and Display
- The menus and HUD show at any resolution. Above 2048 pixels wide (for
  example 2560x1440) the screen used to stay black, or freeze on one menu
  over the game after starting a map.

### Audio
- Sounds no longer bend in pitch as you move: the Doppler effect is off by
  default, since Quake Arena Arcade has none. A config saved earlier keeps
  `s_doppler 1`; set `s_doppler 0` to match the console.

### Builds and Packaging
- Releases are built for Windows x86_64 and Linux x86_64 and hold the engine
  and a setup kit only, never game data or game code.

### Fixes
- The menus are many times faster: the title and menus no longer crawl at
  a few frames a second. They are drawn on every core, and only the parts
  of the screen that changed are drawn again: the main menu runs at full
  frame rate at 2560x1440 instead of about 30 frames a second, and the HUD
  no longer redraws on every frame after the in-game menu.
- Fixed a crash risk when the menus load many images at once: the game's
  image-loading threads and the menus could change the list of images at
  the same time.
- The console no longer opens over the menus at start-up and stays stuck
  there; it starts closed and opens and closes with the console key.
- New players start as James with a red rail, as on the console, and the
  menus' settings start at the console's defaults (weapon autoswitch on,
  music at 0.6). The White rail colour in Player Settings can now be
  chosen, and the HUD places its kill messages for widescreen displays.
- Select Model now shows every character's portrait, and Change
  Arena/Gametype lists the arenas by name with their level shots,
  suggested players and items; both lists were empty. Run setup again
  after updating so the menus are recompiled with the fix.
- Start Match in System Link's Host Match now starts the match, with the
  arena, game type, limits, bot fill and bot skill you chose; it did
  nothing. There is no lobby yet: the game starts at once as a listen
  server that others on your network join with `connect`. The Contestants
  setting now sets the server's size (at most 16), as on the console.
- Pressing Escape in a game no longer crashes the game when the key is
  still held as the in-game menu opens.

### Documentation and Tooling
- The setup checks for every tool the recompile needs before it starts, and
  says how to install any that are missing.
