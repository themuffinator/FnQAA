#!/usr/bin/env python3
"""Recover the names of pak0.bpk entries from their 64-bit hashes.

The hash (code: default.xex lookup 0x82120C98, table setup 0x82124EF0):
  - the path is relative to baseq3, '/' and '\\' become '\\', and it is
    lowercased with an ASCII tolower (0x8212AAB8 -> 0x821DCAF0);
  - h = 0xFAC432B10CD5E44A; for each byte c (sign-extended):
        h = (h << 8) ^ T[h >> 56] ^ c         (64-bit)
  - T[i] is the carry-less product of i and 0x0060034000F0D50B, mod 2^64.

Candidates come from every path-like string in the decoded images and in
the archive's own text, models, maps and Flash movies, expanded by swapping
extensions, until no new name is found.

  python tools/bpk_names.py <pak0.bpk> --images ref/pe/*.pe -o data/bpk/pak0.names
"""
import argparse
import ctypes
import itertools
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

M64 = (1 << 64) - 1
POLY = 0x0060034000F0D50B
SEED = 0xFAC432B10CD5E44A
BACKSLASH = "\\"


def _table():
    t = []
    for i in range(256):
        c, b = 0, i
        for _ in range(8):
            c = (c << 1) & M64
            if b & 0x80:
                c ^= POLY
            b <<= 1
        t.append(c)
    return t


TABLE = _table()


def normalize(path):
    out = []
    for ch in path:
        if ch == "/":
            ch = BACKSLASH
        elif "A" <= ch <= "Z":
            ch = chr(ord(ch) + 32)
        out.append(ch)
    return "".join(out)


def name_hash(path):
    h = SEED
    for b in normalize(path).encode("latin-1", "replace"):
        c = b - 0x100 if b >= 0x80 else b
        h = ((h << 8) & M64) ^ TABLE[h >> 56] ^ (c & 0xFFFFFFFF)
    return h


EXTS = ["tga", "jpg", "dds", "png", "wav", "ogg", "mp3", "md3", "mdc", "skin", "bsp",
        "aas", "cfg", "txt", "shader", "menu", "gfx", "swf", "arena", "bot", "c", "h",
        "dat", "roq", "dm_68", "dm_73", "bik", "wmv", "xma", "ttf", "fontdat", "pcx",
        "md5", "iqm", "lst", "def", "fx", "hlsl", "xpr", "xmv", "efx", "ent", "ini",
        "lang", "xml", "html", "sav"]

# pixel and sample data hold no paths
SKIP_PREFIXES = (b"DDS ", b"RIFF", b"\x89PNG", b"\xff\xd8\xff", b"\x00\x00\x02\x00", b"\x00\x00\x0a\x00")

TOKEN = re.compile(rb"[A-Za-z0-9_\-+.!$&'()\[\]{}/\\ ]{3,}")


def tokens_of(data):
    for m in TOKEN.finditer(data):
        t = m.group().decode("latin-1").strip()
        if t:
            yield t


def expand(tok):
    """The token as a path, and the same path with every known extension."""
    t = tok.replace(BACKSLASH, "/").strip("\"' ")
    while t.startswith("./") or t.startswith("/"):
        t = t[2:] if t.startswith("./") else t[1:]
    if t.lower().startswith("baseq3/"):
        t = t[7:]
    if not t:
        return
    yield t
    stem, dot, ext = t.rpartition(".")
    base = stem if dot and "/" not in ext and len(ext) <= 6 else t
    for e in EXTS:
        yield base + "." + e


def candidates(data):
    for tok in tokens_of(data):
        if "/" in tok or BACKSLASH in tok:
            yield from expand(tok)
        elif "." in tok:
            yield tok


WORD = re.compile(rb"[A-Za-z0-9_\-]{2,}")
SKYPARMS = re.compile(rb"(?i)skyparms\s+([^\s\"]+)")

SEED_DIRS = ["", "maps", "levelshots", "scripts", "botfiles", "botfiles/bots", "ui", "ui/lang",
             "fonts", "gfx", "gfx/2d", "sound", "music", "video", "demos", "models", "sprites",
             "icons", "shaders", "textures", "menu", "ui/assets"]
COMMON_SKINS = ["default", "red", "blue", "bright", "dead", "sport", "sport_red", "sport_blue",
                "sport_bright", "pm", "light", "dark", "white", "gold", "green", "yellow"]
SHORT_EXTS = ["tga", "jpg", "dds", "png", "wav", "md3", "skin", "cfg", "c", "txt", "shader",
              "menu", "gfx", "bsp", "aas", "arena", "bot"]


def templates(names, words, basenames):
    """Candidate names built from the names found so far and loose words."""
    names = list(names)
    dirs = set(SEED_DIRS)
    children = {}
    for n in names:
        d = n.lower().replace(BACKSLASH, "/")
        parts = d.split("/")
        for i in range(1, len(parts)):
            dirs.add("/".join(parts[:i]))
        if len(parts) >= 2:
            parent, leaf = "/".join(parts[:-2]), parts[-2]
            children.setdefault(parent, set()).add(leaf)

    # file names seen anywhere, in every known directory
    for d in dirs:
        pre = d + "/" if d else ""
        for bn in basenames:
            yield pre + bn

    # a file found in one directory is tried in all its sibling directories
    by_dir = {}
    for n in names:
        d, _, f = n.lower().replace(BACKSLASH, "/").rpartition("/")
        by_dir.setdefault(d, set()).add(f)
    for d, files in by_dir.items():
        parent, _, _ = d.rpartition("/")
        for sib in children.get(parent, ()):
            pre = (parent + "/" if parent else "") + sib + "/"
            for f in files:
                yield pre + f

    # maps, their bot files and level shots, and bot scripts
    for w in words:
        for t in ("maps/%s.bsp", "maps/%s.aas", "levelshots/%s.tga", "levelshots/%s.jpg",
                  "levelshots/%s.dds", "scripts/%s.arena", "scripts/%s.txt", "scripts/%s.shader",
                  "botfiles/bots/%s_c.c", "botfiles/bots/%s_i.c", "botfiles/bots/%s_t.c",
                  "botfiles/bots/%s_w.c", "botfiles/%s.c", "botfiles/%s.h", "ui/%s.menu",
                  "ui/%s.gfx", "ui/%s.txt", "ui/lang/%s.txt", "fonts/%s.gfx", "fonts/%s.txt",
                  "%s.cfg", "%s.txt", "music/%s.wav", "video/%s.wmv", "video/%s.bik"):
            yield t % w

    # every loose word as a sound in each directory that holds sounds
    for d in dirs:
        if d.startswith("sound/") or d == "sound" or d == "music":
            for w in words:
                yield "%s/%s.wav" % (d, w)

    # player models: parts, level-of-detail models and skins
    players = children.get("models/players", set())
    for p in players:
        pre = "models/players/%s/" % p
        skins = set(COMMON_SKINS)
        for f in by_dir.get("models/players/" + p, ()):
            stem = f.rsplit(".", 1)[0]
            bits = stem.split("_")
            for i in range(len(bits)):
                for j in range(i + 1, len(bits) + 1):
                    skins.add("_".join(bits[i:j]))
        for part in ("head", "upper", "lower"):
            yield pre + part + ".md3"
            for lod in ("_1", "_2", "_3"):
                yield pre + part + lod + ".md3"
            for s in skins:
                yield pre + "%s_%s.skin" % (part, s)
        yield pre + "animation.cfg"
        for s in skins:
            for e in ("tga", "dds", "jpg"):
                yield pre + "icon_%s.%s" % (s, e)
            for e in SHORT_EXTS[:3]:
                yield pre + "%s.%s" % (s, e)
                yield pre + "%s_%s.%s" % (p, s, e)


FORMAT = re.compile(rb"[A-Za-z0-9_\-./\\:%]*%[0-9]*[sdi][A-Za-z0-9_\-./\\:%]*")
SLOT = re.compile(r"%(0?[0-9]*)([sdi])")
LANGUAGES = ["english", "french", "german", "italian", "spanish", "japanese", "korean",
             "chinese", "russian", "polish", "portuguese", "dutch",
             "en", "fr", "de", "it", "es", "ja", "jp", "ko", "zh", "ru", "pl", "pt", "nl",
             "en-us", "fr-fr", "de-de", "it-it", "es-es", "ja-jp", "en_us", "fr_fr", "de_de",
             "it_it", "es_es", "ja_jp", "us", "uk", "eng", "fre", "ger", "ita", "spa", "jpn"]


def fill_formats(formats, names):
    """Fill %s from parts of known names (fewer for multi-slot formats), %d from 0..99."""
    parts = {}
    for n in names:
        for p in re.split(r"[/\\._]", n.lower()):
            if p:
                parts[p] = parts.get(p, 0) + 1
        segs = n.lower().replace(BACKSLASH, "/").split("/")
        for s in segs:
            parts[s] = parts.get(s, 0) + 1
            stem = s.rsplit(".", 1)[0]
            parts[stem] = parts.get(stem, 0) + 1
    for lang in LANGUAGES:
        parts[lang] = parts.get(lang, 0) + 1000
    parts[""] = 1000
    ranked = sorted(parts, key=lambda p: -parts[p])
    vocab = {1: ranked, 2: ranked[:1500], 3: ranked[:120], 4: ranked[:40]}
    numbers = {}
    for f in formats:
        slots = SLOT.findall(f)
        if not slots or len(slots) > 4:
            continue
        n_s = sum(1 for _, t in slots if t == "s")
        choices = []
        for width, t in slots:
            if t == "s":
                choices.append(vocab[max(1, n_s)])
            else:
                key = width
                if key not in numbers:
                    fmt = "%" + width + "d"
                    numbers[key] = [fmt % i for i in range(100)]
                choices.append(numbers[key])
        pieces = SLOT.split(f)
        literal = pieces[0::3]
        for combo in itertools.product(*choices):
            out = [literal[0]]
            for i, v in enumerate(combo):
                out.append(v)
                out.append(literal[i + 1])
            s = "".join(out).lstrip("/")
            if s.lower().startswith("baseq3/"):
                s = s[7:]
            yield s


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("bpk")
    ap.add_argument("--images", nargs="*", default=[])
    ap.add_argument("--seeds", help="extra candidate names, one per line")
    ap.add_argument("-o", "--out")
    a = ap.parse_args()

    from bpk import Bpk
    b = Bpk(a.bpk)
    wanted = {e[0] for e in b.entries}
    found = {}
    stats = {"tried": 0}

    lib = b.lzx
    lib.qaa_bpk_name_hash_many.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint64)]

    def flush(batch, new):
        packed = b"".join(c.encode("latin-1", "replace") + b"\0" for c in batch)
        out = (ctypes.c_uint64 * len(batch))()
        lib.qaa_bpk_name_hash_many(packed, len(batch), out)
        stats["tried"] += len(batch)
        for c, h in zip(batch, out):
            if h in wanted and h not in found:
                found[h] = c
                new.append(h)

    def attempt(cands):
        """Hash candidates in batches, streaming: nothing is kept but the hits."""
        new, batch, seen = [], [], set()
        for c in cands:
            if c in seen:
                continue
            seen.add(c)
            batch.append(c)
            if len(batch) >= 1 << 16:
                flush(batch, new)
                batch, seen = [], set()
        if batch:
            flush(batch, new)
        return new

    sources = [open(img, "rb").read() for img in a.images]
    seeds = []
    if a.seeds:
        seeds = [line.strip() for line in open(a.seeds, encoding="latin-1") if line.strip()]
    for e in b.entries:
        data = b.read(e)
        if not data.startswith(SKIP_PREFIXES):
            sources.append(data)

    # phase 1: literal paths, and the same paths with other extensions
    for data in sources:
        attempt(candidates(data))
    for s in seeds:
        attempt(expand(s))

    # sky boxes: "skyParms <base> ..." names six images, <base>_rt and so on
    for data in sources:
        for m in SKYPARMS.finditer(data):
            base = m.group(1).decode("latin-1")
            for side in ("rt", "lf", "bk", "ft", "up", "dn"):
                attempt(expand("%s_%s.tga" % (base, side)))

    # words and file names seen anywhere, for the templates below
    words, basenames = set(), set()
    for data in sources:
        for m in WORD.finditer(data):
            w = m.group().decode("latin-1").lower()
            if len(w) <= 40:
                words.add(w)
        for tok in tokens_of(data):
            t = tok.replace(BACKSLASH, "/").rsplit("/", 1)[-1].lower()
            if "." in t and 2 < len(t) <= 64:
                basenames.add(t)
    print("phase 1: %d named; %d words, %d file names" % (len(found), len(words), len(basenames)),
          file=sys.stderr)

    # phase 2: templates over what is known so far, repeated to a fixed point
    rounds = 0
    while rounds < 4:
        rounds += 1
        before = len(found)
        attempt(templates(found.values(), words, basenames))
        print("phase 2 round %d: %d named" % (rounds, len(found)), file=sys.stderr)
        if len(found) == before:
            break

    # phase 3: the binaries' own path format strings, filled from known parts
    formats = set()
    for data in sources[:len(a.images)]:
        for m in FORMAT.finditer(data):
            f = m.group().decode("latin-1")
            if "/" in f or BACKSLASH in f or re.search(r"\.[a-z0-9]{2,5}$", f):
                formats.add(f.replace(BACKSLASH, "/"))
    attempt(fill_formats(formats, found.values()))
    print("phase 3: %d formats, %d named" % (len(formats), len(found)), file=sys.stderr)

    print("%d of %d named (%d candidates, %d rounds)" % (len(found), len(wanted), stats["tried"], rounds),
          file=sys.stderr)
    if a.out:
        os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
        with open(a.out, "w", newline="\n") as o:
            o.write("\n".join(sorted("%016x %s" % (h, n) for h, n in found.items())) + "\n")
    return 0 if len(found) == len(wanted) else 2


if __name__ == "__main__":
    sys.exit(main())
