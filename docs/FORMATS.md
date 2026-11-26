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
- **observed** Names are stored only as 64-bit hashes; the hash function is
  not yet known (M1).
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
