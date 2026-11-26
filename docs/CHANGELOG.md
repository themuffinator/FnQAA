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
- The engine reads the game's data inside your package, in place, and never
  changes it; saved games and title updates stored beside the game are left
  alone.

### Rendering and Display
- _None yet._

### Audio
- _None yet._

### Builds and Packaging
- Releases are built for Windows x86_64 and Linux x86_64 and hold the engine
  and a setup kit only, never game data or game code.

### Fixes
- _None yet._

### Documentation and Tooling
- The setup checks for every tool the recompile needs before it starts, and
  says how to install any that are missing.
