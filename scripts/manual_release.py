"""FnQAA manual release helper, after FnQL's scripts/manual_release.py.

Versioning (FnQL's manual channel):
  - version/fnqaa_version.h holds <major>.<minor>.<patch>; a release's build
    number is the number of commits since the latest stable tag
    v<major>.<minor>.<patch> (every commit when there is none), and the
    release version is <major>.<minor>.<patch>.<build>.
  - the release tag is <version>-<YYYYMMDD>-<commit>, the title
    "FnQAA <date> (<version>)", the archives fnqaa-<tag>-<platform>-<arch>.
  - stamp-version writes the build number into the header before a release
    build, so the binaries report the release version.

Release notes (FnQL's changelog management):
  - a tracked file docs/releases/<tag>.md or <version>.md wins;
  - else the Unreleased queue of docs/CHANGELOG.md, published as written
    (Highlights first, then every other entry under its category);
  - else highlights made from the commit subjects in the release range;
  followed by the build details and the included commits. After a published
  release the workflow resets the queue (changelog.py clear-unreleased).

  manual_release.py summary | github-output [--build-date YYYY-MM-DD] [--head-commit SHA]
  manual_release.py stamp-version --build-number N
  manual_release.py release-notes --build-number N [--from-commit SHA] [--to-commit SHA] [--output FILE]

FnQL's GitHub Models highlights, FnQL-game pin and Discord announcement are
not part of FnQAA.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from fnqaa_meta import (  # noqa: E402
    ROOT,
    VERSION_HEADER,
    base_metadata,
    channel_metadata,
    compose_version_string,
    compose_windows_version,
    normalize_commit,
    normalize_date,
)
from changelog import (  # noqa: E402
    CHANGELOG_CATEGORIES,
    DEFAULT_CHANGELOG,
    clear_unreleased,
    items_by_category,
    section_items,
)

DEFAULT_CURATED_RELEASE_NOTES_DIRECTORY = ROOT / "docs" / "releases"
FIELD_SEPARATOR = "\x1f"
RECORD_SEPARATOR = "\x1e"
# Highlights shown when the changelog has no Highlights entries: the first
# entry of each other category, at most this many.
MAX_PROMOTED_HIGHLIGHTS = 5
# Commits that only maintain the release machinery itself.
BOT_AUTHOR_SUFFIX = "[bot]"
HOUSEKEEPING_SUBJECTS = (
    "reset pending changelog after release",
    "reset unreleased changelog",
)


def non_negative_int(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be an integer") from exc
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be non-negative")
    return parsed


def write_text_lf(path: Path, content: str) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(content)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="FnQAA manual release build helper")
    subparsers = parser.add_subparsers(dest="command", required=True)

    for command in ("summary", "github-output"):
        subparser = subparsers.add_parser(command)
        subparser.add_argument("--build-date")
        subparser.add_argument("--head-commit")

    stamp = subparsers.add_parser("stamp-version")
    stamp.add_argument("--build-number", required=True, type=non_negative_int)
    stamp.add_argument("--header", type=Path, default=VERSION_HEADER)

    notes = subparsers.add_parser("release-notes")
    notes.add_argument("--build-number", required=True, type=non_negative_int)
    notes.add_argument("--build-date")
    notes.add_argument("--from-commit")
    notes.add_argument("--to-commit")
    notes.add_argument("--changelog", type=Path, default=DEFAULT_CHANGELOG)
    notes.add_argument(
        "--curated-notes-dir",
        type=Path,
        default=DEFAULT_CURATED_RELEASE_NOTES_DIRECTORY,
        help="Directory containing optional tracked release notes (default: docs/releases)",
    )
    notes.add_argument("--output", type=Path)
    notes.add_argument(
        "--clear-changelog",
        action="store_true",
        help="Reset the Unreleased changelog section after release notes are written",
    )

    return parser.parse_args(argv)


def git(*args: str, check: bool = True) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=ROOT,
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
        check=False,
    )
    if check and result.returncode != 0:
        message = result.stderr.strip() or result.stdout.strip() or "git command failed"
        raise RuntimeError(message)
    return result.stdout.strip()


def resolve_tag_commit(tag_name: str) -> str:
    return git("rev-parse", f"refs/tags/{tag_name}^{{commit}}", check=False)


def latest_tag(pattern: str) -> str:
    output = git("tag", "--list", pattern, "--sort=-creatordate", check=False)
    tags = [line.strip() for line in output.splitlines() if line.strip()]
    return tags[0] if tags else ""


def latest_stable_tag(tag_prefix: str) -> str:
    pattern = f"{tag_prefix}[0-9]*"
    output = git("tag", "--list", pattern, "--sort=-version:refname", check=False)
    tags = [line.strip() for line in output.splitlines() if line.strip()]
    return tags[0] if tags else ""


def manual_release_tag_pattern(base_version: str) -> str:
    return f"{base_version}.*-????????-*"


def commit_count_since(stable_tag: str, head_commit: str) -> int:
    range_spec = head_commit if not stable_tag else f"{stable_tag}..{head_commit}"
    count = int(git("rev-list", "--count", range_spec) or "0")
    return max(1, count)


def manual_release_context(
    *,
    build_date: str | None = None,
    head_commit: str | None = None,
) -> dict[str, object]:
    meta = base_metadata()
    head_sha = (head_commit or git("rev-parse", "HEAD")).strip()
    iso_date, _ = normalize_date(build_date)
    latest_release = latest_tag(manual_release_tag_pattern(str(meta["base_version"])))
    previous_release_commit = resolve_tag_commit(latest_release) if latest_release else ""
    stable_tag = latest_stable_tag(str(meta["tag_prefix"]))
    build_number = commit_count_since(stable_tag, head_sha)
    version_string = compose_version_string(
        int(meta["version_major"]),
        int(meta["version_minor"]),
        int(meta["version_patch"]),
        build_number,
    )
    release = channel_metadata("manual", build_number=build_number, build_date=iso_date, commit=head_sha)

    return {
        "project_name": str(meta["project_name"]),
        "base_version": str(meta["base_version"]),
        "version_string": version_string,
        "build_number": build_number,
        "build_date": iso_date,
        "head_commit": head_sha,
        "head_commit_short": normalize_commit(head_sha),
        "release_tag": release["release_tag"],
        "release_title": release["release_title"],
        "archive_prefix": release["archive_prefix"],
        "latest_release_tag": latest_release,
        "stable_tag": stable_tag,
        "previous_release_commit": previous_release_commit,
    }


def print_mapping(data: dict[str, object]) -> None:
    for key, value in data.items():
        rendered = str(value).lower() if isinstance(value, bool) else str(value)
        print(f"{key}={rendered}")


def stamp_version(header: Path, build_number: int) -> dict[str, object]:
    meta = base_metadata(header)
    parts = (int(meta["version_major"]), int(meta["version_minor"]), int(meta["version_patch"]), build_number)
    version_string = compose_version_string(*parts)
    windows_version = compose_windows_version(*parts)

    replacements = {
        "FNQAA_VERSION_TWEAK": str(build_number),
        "FNQAA_VERSION_STRING": f'"{version_string}"',
        "FNQAA_WINDOWS_FILE_VERSION": windows_version,
        "FNQAA_WINDOWS_PRODUCT_VERSION": windows_version,
    }

    lines = header.read_text(encoding="utf-8").splitlines()
    seen: set[str] = set()
    rewritten: list[str] = []

    for line in lines:
        fields = line.strip().split(maxsplit=2)
        if len(fields) >= 3 and fields[0] == "#define" and fields[1] in replacements:
            key = fields[1]
            rewritten.append(f"#define {key} {replacements[key]}")
            seen.add(key)
        else:
            rewritten.append(line)

    missing = sorted(set(replacements) - seen)
    if missing:
        raise KeyError(f"Missing version defines in {header}: {', '.join(missing)}")

    write_text_lf(header, "\n".join(rewritten) + "\n")
    return {"version_string": version_string, "build_number": build_number}


def release_range_spec(from_commit: str | None, to_commit: str) -> str:
    if from_commit and from_commit != to_commit:
        return f"{from_commit}..{to_commit}"
    if from_commit == to_commit:
        return f"{to_commit}^!"

    stable_tag = latest_stable_tag(str(base_metadata()["tag_prefix"]))
    return f"{stable_tag}..{to_commit}" if stable_tag else to_commit


def commit_entries(from_commit: str | None, to_commit: str) -> list[dict[str, str]]:
    range_spec = release_range_spec(from_commit, to_commit)
    pretty = "%H%x1f%h%x1f%an%x1f%ad%x1f%P%x1f%s%x1f%b%x1e"
    log = git("log", "--reverse", "--date=short", f"--pretty=format:{pretty}", range_spec)
    entries: list[dict[str, str]] = []

    for record in log.split(RECORD_SEPARATOR):
        record = record.strip("\n")
        if not record:
            continue
        fields = record.split(FIELD_SEPARATOR, 6)
        if len(fields) != 7:
            continue
        commit_hash, short_hash, author, date, parents, subject, body = fields
        entries.append(
            {
                "hash": commit_hash.strip(),
                "short": short_hash.strip(),
                "author": author.strip(),
                "date": date.strip(),
                "subject": subject.strip(),
                "body": body.strip(),
                "parents": str(len(parents.split())),
            }
        )

    return entries


def is_merge_subject(subject: str) -> bool:
    return subject.lower().startswith(("merge branch ", "merge pull request ", "merge remote-tracking "))


def is_release_housekeeping(entry: dict[str, str]) -> bool:
    """Merge commits and the bot's changelog resets say nothing about a release."""
    subject = entry["subject"]
    if not subject or is_merge_subject(subject):
        return True
    if int(entry.get("parents", "1") or "1") > 1:
        return True
    if entry.get("author", "").endswith(BOT_AUTHOR_SUFFIX):
        return True
    return subject.lower().rstrip(".") in HOUSEKEEPING_SUBJECTS


def commit_lines(from_commit: str | None, to_commit: str) -> list[str]:
    return [
        f"- {entry['short']} {entry['subject']}"
        for entry in commit_entries(from_commit, to_commit)
        if not is_release_housekeeping(entry)
    ]


def changelog_release_body(changelog: Path) -> str:
    """Render the Unreleased queue as release notes, every entry in full.

    A short Highlights section (the Highlights entries, or the first entry of
    each other category when there are none) followed by every other entry
    under its category. Returns "" when the queue has no real entries.
    """
    try:
        items = section_items(changelog, "Unreleased")
    except (OSError, KeyError):
        return ""
    if not items:
        return ""

    by_category = items_by_category(items)
    highlights = list(by_category.get("Highlights", []))
    remaining = {
        category: list(by_category.get(category, []))
        for category in CHANGELOG_CATEGORIES
        if category != "Highlights"
    }
    if not highlights:
        for category, entries in remaining.items():
            if entries and len(highlights) < MAX_PROMOTED_HIGHLIGHTS:
                highlights.append(entries.pop(0))

    lines = ["## Highlights", ""]
    lines.extend(f"- {text}" for text in highlights)
    other = [(category, entries) for category, entries in remaining.items() if entries]
    if other:
        lines.extend(["", "## All changes"])
        for category, entries in other:
            lines.extend(["", f"### {category}", ""])
            lines.extend(f"- {text}" for text in entries)
    return "\n".join(lines)


def sanitize_notes(text: str) -> str:
    """Drops a leading title and code fence, and neutralises workflow commands."""
    stripped = text.strip()
    fence = re.fullmatch(r"```(?:markdown|md)?\s*(.*?)\s*```", stripped, flags=re.IGNORECASE | re.DOTALL)
    if fence:
        stripped = fence.group(1).strip()

    sanitized: list[str] = []
    for line in stripped.splitlines():
        clean = line.rstrip()
        if re.match(r"^#{1,2}\s+(fnqaa|release notes|changelog highlights|what'?s changed)\b", clean,
                    flags=re.IGNORECASE):
            continue
        if clean.startswith("::"):
            clean = "\\" + clean
        sanitized.append(clean)

    return "\n".join(sanitized).strip()


def find_curated_release_notes(directory: Path, *, release_tag: str, version_string: str) -> Path | None:
    for name in (f"{release_tag}.md", f"{version_string}.md"):
        candidate = directory / name
        if candidate.is_symlink():
            raise RuntimeError(f"Tracked release notes must not be a symlink: {candidate}")
        if candidate.is_file():
            return candidate
    return None


def read_curated_release_notes(path: Path) -> str:
    content = sanitize_notes(path.read_text(encoding="utf-8"))
    if not content:
        raise ValueError(f"Tracked release notes are empty after removing their title: {path}")
    return content


def normalized_subject_key(subject: str) -> str:
    return re.sub(r"[^a-z0-9]+", " ", subject.lower()).strip()


def humanize_subject(subject: str) -> str:
    cleaned = re.sub(r"\s+\(#\d+\)$", "", subject.strip())
    if not cleaned:
        return cleaned
    cleaned = cleaned[0].upper() + cleaned[1:]
    if cleaned[-1] not in ".!?":
        cleaned += "."
    return cleaned


def fallback_highlights(commits: list[dict[str, str]]) -> str:
    """Commit-subject highlights, for a release whose changelog queue is empty."""
    seen: set[str] = set()
    rendered: list[str] = []

    for entry in commits:
        subject = entry["subject"]
        if is_release_housekeeping(entry) or subject.lower().startswith(("docs:", "doc:")):
            continue
        key = normalized_subject_key(subject)
        if key in seen:
            continue
        seen.add(key)
        rendered.append(f"- {humanize_subject(subject)}")
        if len(rendered) == 12:
            break

    return "\n".join(rendered) or "- No user-facing changes were detected in this release range."


# Every release says this: what it holds and how a user sets it up (README.md, Install).
GETTING_STARTED = """## Getting started

FnQAA runs Quake Arena Arcade from your own copy of the game. The archives hold the engine and a setup kit, and no game data or game code.

1. Download the archive for your platform and extract it.
2. Put your copy of the game in that folder, as you have it: the XBLA zip (`Quake Arena Arcade (World) (XBLA).zip`) or the zip extracted, the `Content` folder copied from your Xbox 360 or its storage device, or the package file `0ED9DD87...` itself. Nothing else needs extracting: the game's data is read inside the package, in place, and never changed.
3. Run `setup.cmd` (Windows) or `./setup.sh` (Linux) once. It finds the game, recompiles its game code (a few minutes) and installs it in the folder. It checks first for what it needs and says how to install anything missing: Python 3 with `cryptography`, git, CMake, Ninja and clang (LLVM), plus Visual Studio 2022 or its Build Tools with the C++ workload on Windows.
4. Start `fnqaa`.

Audio needs OpenAL Soft (`soft_oal.dll` or `OpenAL32.dll` on Windows, `libopenal.so.1` on Linux). Each archive's INSTALL.txt repeats these steps, and SHA256SUMS.txt lists the archives' checksums."""


def render_release_notes(
    *,
    build_number: int,
    build_date: str | None = None,
    from_commit: str | None = None,
    to_commit: str | None = None,
    changelog: Path = DEFAULT_CHANGELOG,
    curated_notes_dir: Path = DEFAULT_CURATED_RELEASE_NOTES_DIRECTORY,
) -> str:
    meta = base_metadata()
    target_commit = (to_commit or git("rev-parse", "HEAD")).strip()
    iso_date, _ = normalize_date(build_date)
    version_string = compose_version_string(
        int(meta["version_major"]), int(meta["version_minor"]), int(meta["version_patch"]), build_number,
    )
    release_tag = str(
        channel_metadata("manual", build_number=build_number, build_date=iso_date, commit=target_commit)["release_tag"]
    )
    curated_path = find_curated_release_notes(curated_notes_dir, release_tag=release_tag, version_string=version_string)
    changelog_body = "" if curated_path is not None else changelog_release_body(changelog)
    if curated_path is not None:
        release_body = read_curated_release_notes(curated_path)
        print(f"Using tracked release notes from {curated_path}", file=sys.stderr)
    elif changelog_body:
        release_body = changelog_body
        print(f"Using the Unreleased queue from {changelog}", file=sys.stderr)
    else:
        release_body = "## Highlights\n\n" + fallback_highlights(commit_entries(from_commit, target_commit))
        print("The Unreleased queue is empty: highlights come from the commit subjects", file=sys.stderr)

    if from_commit:
        previous_line = f"- Previous release commit: {normalize_commit(from_commit)} ({from_commit})"
    else:
        stable_tag = latest_stable_tag(str(meta["tag_prefix"]))
        previous_line = f"- Previous stable tag: {stable_tag}" if stable_tag else "- Previous release commit: none"

    # GitHub shows the release title above the body, so the body has none.
    lines = [
        release_body,
        "",
        GETTING_STARTED,
        "",
        "## Build details",
        "",
        f"- Version: {version_string}",
        f"- Build date: {iso_date}",
        f"- Commit: {normalize_commit(target_commit)} ({target_commit})",
        previous_line,
        "- Every test ran headless on both platforms before packaging.",
    ]

    commits = commit_lines(from_commit, target_commit)
    lines.extend(["", "<details>", "<summary>Included commits</summary>", ""])
    if commits:
        lines.extend(commits)
    else:
        lines.append(f"- {normalize_commit(target_commit)} no new commits were found for the requested range")
    lines.extend(["", "</details>"])

    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)

    if args.command in {"summary", "github-output"}:
        print_mapping(manual_release_context(build_date=args.build_date, head_commit=args.head_commit))
        return 0

    if args.command == "stamp-version":
        print_mapping(stamp_version(args.header, args.build_number))
        return 0

    notes = render_release_notes(
        build_number=args.build_number,
        build_date=args.build_date,
        from_commit=args.from_commit,
        to_commit=args.to_commit,
        changelog=args.changelog,
        curated_notes_dir=args.curated_notes_dir,
    )
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        write_text_lf(args.output, notes)
    else:
        sys.stdout.write(notes)
    if args.clear_changelog:
        clear_unreleased(args.changelog)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
