# Formats

Each claim is marked **verified** (a tool exercises it over the whole
package), **code** (read from named retail code) or **observed** (seen in
samples, not yet checked everywhere).

## The package

The user's copy is `Quake Arena Arcade (World) (XBLA).zip`, holding one file:
`584108C8/000D0000/0ED9DD87783BD72BA56ED67ACD52BBFF1F47F08458`.

### STFS (`tools/stfs.py`)

- **verified** Magic `LIVE`, title ID `584108C8`, content type `000D0000`
  (Xbox Live Arcade), header size 0xAD0E, so data blocks start at 0xB000.
- **verified** Volume descriptor flags bit 0 is set: one hash table per
  level (read-only package).
- **verified** The file table has 20 entries in one block. All files decode.

| Path | Size |
|---|---|
| `default.xex` | 3,526,656 |
| `qagameXe.dll` | 462,848 |
| `cgameXe.dll` | 401,408 |
| `baseq3/pak0.bpk` | 374,766,959 |
| `ArcadeInfo.xml` | 2,972 (UTF-16; 12 achievements, one leaderboard) |
| `title.png`, `rating_esrb_m.png`, 12 `ach_*.png` | images |

## XEX2 (`tools/xex.py`)

- **verified** All three images are encrypted (type 1). The session key is
  the security info's AES key decrypted with the retail key; the payload is
  then AES-128-CBC with a zero IV.
- **verified** `qagameXe.dll` and `cgameXe.dll` use basic compression
  (data and zero runs); `default.xex` uses normal compression (one raw LZX
  stream, window from the file format info).
- **verified** The decoded images are in loaded layout: offset = VA - base.
  The `.edata` and `.idata` sections the PE headers list lie beyond the
  stored data; exports come from the XEX export table instead.

| Image | Original name | Base | Image size | Entry | Imports |
|---|---|---|---|---|---|
| `default.xex` | `quake3xe.exe` | 0x82000000 | 0x01040000 | 0x821D7520 | xboxkrnl 313, xam 182 records |
| `qagameXe.dll` | `qagame.dll` | 0x88000000 | 0x001C0000 | 0x880556F0 | xboxkrnl 83 records (43 functions) |
| `cgameXe.dll` | `cgame.dll` | 0x86000000 | 0x003A0000 | 0x86044800 | xboxkrnl 100 records (52 functions) |

- **verified** Module exports (ordinal: address):
  qagame 1: 0x88042158, 2: 0x8804C990; cgame 1: 0x86020E68, 2: 0x86031D88.
- **observed** `default.xex` loads modules by the patterns `game:\%sXe.dll`
  and `game:\%sXe%s.dll`.

### Module kernel imports (verified, ordinals named from xenia's table)

Both: KeBugCheck, KeBugCheckEx, KeDebugMonitorData, KeGetCurrentProcessType,
NtAllocateVirtualMemory, NtClose, NtCreateFile, NtDuplicateObject,
NtFlushBuffersFile, NtFreeVirtualMemory, NtOpenFile, NtQueryDirectoryFile,
NtQueryFullAttributesFile, NtQueryInformationFile, NtQueryVirtualMemory,
NtQueryVolumeInformationFile, NtReadFile, NtReadFileScatter,
NtSetInformationFile, NtWaitForSingleObjectEx, NtWriteFile,
RtlCompareMemoryUlong, RtlEnterCriticalSection, RtlFreeAnsiString,
RtlImageXexHeaderField, RtlInitAnsiString, RtlInitUnicodeString,
RtlInitializeCriticalSection, RtlInitializeCriticalSectionAndSpinCount,
RtlLeaveCriticalSection, RtlNtStatusToDosError, RtlRaiseException,
RtlUnicodeStringToAnsiString, KeTlsAlloc, KeTlsFree, KeTlsGetValue,
KeTlsSetValue, XboxHardwareInfo, XexExecutableModuleHandle,
__C_specific_handler.

qagame only: KeQueryPerformanceFrequency, KeQuerySystemTime,
RtlMultiByteToUnicodeN.

cgame only: ExCreateThread, ExTerminateThread, ExThreadObjectType,
KeSetAffinityThread, NtClearEvent, NtCreateEvent, NtResumeThread,
NtSetEvent, NtWaitForMultipleObjectsEx, ObDereferenceObject,
ObReferenceObjectByHandle, RtlTryEnterCriticalSection.

## BPK (`tools/bpk.py`)

- **verified** Big-endian header: `u32 version = 1`, `u32 count = 4916`;
  then `count` records of 24 bytes:
  `u64 name hash, u32 offset, u32 size, u32 flags, u32 stored size`.
  The table ends at 0x1CCE8, where the first file starts.
- **verified** Every record has flags 1, and every stored file is a native
  XMemCompress stream. All 4,916 decode, 1,319,183,938 bytes in total.
- **code** Names are stored only as 64-bit hashes (`code/qaa/qaa_bpk.c`,
  from the lookup at 0x82120C98 and the table setup at 0x82124EF0). The
  path is relative to `baseq3`. Every `/` becomes `\` and A-Z are lowercased
  (ASCII `tolower` at 0x821DCAF0). The state starts at 0xFAC432B10CD5E44A,
  and each byte, sign-extended, is folded in as
  `h = (h << 8) ^ T[h >> 56] ^ c`. `T[i]` is the carry-less product of `i`
  and 0x0060034000F0D50B, mod 2^64; it is not a textbook CRC table.
- **verified** The hash names entries: `default.cfg` hashes to the second
  record, 0xABF374818163C51F. `tools/bpk_names.py` names 4,682 of the 4,913
  distinct hashes (95.3%) from path strings in the three images and the
  archive's own text, models, maps and movies, the binaries' path format
  strings, templates (sibling directories, player parts and skins, sky box
  sides, map and bot file patterns), and optionally the file lists of a local
  Quake III / Quake Live install as seeds. The 231 left are 142 DDS, 58 TGA,
  7 WAV, and the six localisation tables, six font movies, four font maps and
  four controller configs whose names come from format strings such as
  `%s%s/fontmap_%s.txt` with arguments not yet traced.
- **observed** `version.txt` reads `6132 @ Fri, 05 Nov 2010 11:25:50`. The
  45 maps are mpq3ctf1/3/4, mpq3tourney6, mpteam1/2/3/6/7, mptourney1-4,
  q3dm1-7/9/11/14-17, q3tourney1-4, qaactf02/03/05/06, qaadm01/02/03/06/07/
  12-15/18 and qaatraining, each with an AAS file.
- **code** `FS_OpenBpkFile` (0x82120EF8) keeps up to 16 archives in 0x118-byte
  slots at 0x828FB6F0: in-use flag +0, handle +4, name +8 (0x100 bytes),
  running entry base +0x108, version +0x10C, count +0x110, table +0x114.
  The lookup scans the table linearly, newest archive first, and compares
  the high and low 32-bit halves. A hit returns the record's uncompressed
  size; the file handle number is the archive's running entry base, plus
  the record index, plus 0x40.
- **code** Retail cannot list a BPK's contents. Seven functions touch the
  archive slots (open 0x82120EF8, close 0x82120E58, lookup 0x82120C98,
  read 0x82121E00, shutdown 0x82123F90, startup 0x82124128 and an archive
  name check at 0x8218C2E0), and none enumerates entries. Files in a BPK
  are reached only by exact name, so openQAA keeps them out of directory
  listings too. The names `tools/bpk_names.py` recovers are for tools and
  diagnostics; they are generated from the user's copy into `ref/`, never
  committed.
- **verified** 4,916 records hold 4,913 distinct hashes; 0x5639940810D5CC18,
  0x56399F5B10D5CC18 and 0x3D74EED49587C6E0 each appear twice, both copies
  with identical stored bytes, so which one a lookup finds does not matter.
- **observed** Contents by magic: 1,962 DDS (DXT5 1,548, DXT3 296, DXT1 112,
  DXT2 3, other 3), 670 uncompressed TGA, 662 WAV (22,050 Hz mono seen),
  442 MD3 (`IDP3` v15), 304 JPEG, 54 Scaleform `GFX` (version 9), 45 BSP
  (`IBSP` v47), 45 AAS (`EAAS` v5), and text: shaders, `.menu`, `.skin`,
  animation configs, bot files.

## XMemCompress / LZX (`code/qaa/qaa_lzx.c`)

- **verified** Native stream header (big-endian, 0x30 bytes):
  `u32 0x0FF512EE, u16 version 0x0103, u16 reserved, u32 context flags,
  u32 flags, u32 window size (0x8000), u32 partition size (0x8000),
  u64 uncompressed size, u64 compressed size, u32 uncompressed block size
  (0x8000), u32 largest compressed block`.
- **verified** Then blocks: `u32 BE compressed size` and its data. Each block
  is an independent LZX stream (all decoder state resets) and holds chunks,
  one LZX frame each: a `u16 BE` compressed size (frame is 32 KiB), or
  `0xFF, u16 BE frame size, u16 BE compressed size`.
- **verified** LZX: window 2^15 (30 position slots), 24-bit block lengths
  read as 16 + 8 bits, no E8 translation; the bit reader restarts at every
  chunk.
- **verified** XEX2 normal compression joins its blocks' chunks into one raw
  LZX stream with no frame prefixes; each 32 KiB frame ends on a 16-bit
  boundary. `default.xex` decodes this way to its full 0x01040000 image.
