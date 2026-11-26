#!/usr/bin/env python3
"""Tests for the release tooling: FnQL-style versioning and changelog
(scripts/fnqaa_meta.py, changelog.py, manual_release.py), packaging rules
(scripts/package_release.py) and the setup kit's package discovery
(tools/recomp/setup_env.py). Synthetic data only; nothing here reads a game
package. Adapted from FnQL's changelog, release-notes and version tests.

    python tests/release_scripts_tests.py -v
"""
from __future__ import annotations

import json
import shutil
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tools" / "recomp"))

import changelog  # noqa: E402
import fnqaa_meta  # noqa: E402
import manual_release  # noqa: E402
import package_release  # noqa: E402
import setup_env  # noqa: E402

QUEUE = """# Changelog

Intro text.

## [Unreleased]

### Highlights
- _None yet._

### Compatibility
- The first compatibility entry, wrapped
  over two lines.
- A second compatibility entry.

### Audio
- An audio entry.

### Fixes
- _None yet._

## [0.0.1] - 2026-01-01

### Fixes
- An old entry.
"""


class TempDir(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="fnqaa-release-tests-"))

    def tearDown(self):
        shutil.rmtree(self.tmp)


class ChangelogTests(TempDir):
    def write(self, text=QUEUE):
        path = self.tmp / "CHANGELOG.md"
        path.write_text(text, encoding="utf-8")
        return path

    def test_wrapped_entries_are_read_whole(self):
        items = changelog.section_items(self.write(), "Unreleased")
        self.assertIn(("Compatibility", "The first compatibility entry, wrapped over two lines."), items)
        self.assertEqual(len(items), 3)

    def test_clear_unreleased_resets_to_the_empty_template(self):
        path = self.write()
        changelog.clear_unreleased(path)
        self.assertEqual(changelog.section_items(path, "Unreleased"), [])
        text = path.read_text(encoding="utf-8")
        for category in changelog.CHANGELOG_CATEGORIES:
            self.assertIn("### " + category, text)
        self.assertIn("## [0.0.1] - 2026-01-01", text, "older sections are kept")

    def test_prepare_release_moves_whole_entries(self):
        path = self.write()
        changelog.prepare_release(path, "0.0.2.30", "2026-11-26")
        self.assertEqual(changelog.section_items(path, "Unreleased"), [])
        moved = changelog.section_items(path, "0.0.2.30")
        self.assertEqual(len(moved), 3)

    def test_prepare_release_rejects_unsafe_labels_and_dates(self):
        path = self.write()
        with self.assertRaises(ValueError):
            changelog.prepare_release(path, "../x", "2026-11-26")
        with self.assertRaises(ValueError):
            changelog.prepare_release(path, "0.0.3", "26/11/2026")

    def test_the_repository_queue_parses(self):
        changelog.section_items(changelog.DEFAULT_CHANGELOG, "Unreleased")


class VersionTests(TempDir):
    def test_manual_tag_title_and_archive_prefix(self):
        meta = fnqaa_meta.channel_metadata("manual", build_number=41, build_date="2026-11-26",
                                           commit="3f80a8a1b671090dbd80f962e11c7a267ea6b20b")
        base = fnqaa_meta.base_metadata()["base_version"]
        self.assertEqual(meta["version"], f"{base}.41")
        self.assertEqual(meta["release_tag"], f"{base}.41-20261126-3f80a8a1")
        self.assertEqual(meta["release_title"], f"FnQAA 2026-11-26 ({base}.41)")
        self.assertEqual(meta["archive_prefix"], f"fnqaa-{base}.41-20261126-3f80a8a1")
        name = package_release.archive_name("windows", meta["archive_prefix"])
        self.assertTrue(package_release.ARCHIVE_NAME.match(name), name)

    def test_commits_must_be_hex_and_build_numbers_non_negative(self):
        self.assertEqual(fnqaa_meta.normalize_commit(None), "local")
        with self.assertRaises(ValueError):
            fnqaa_meta.normalize_commit("../etc")
        with self.assertRaises(ValueError):
            fnqaa_meta.channel_metadata("manual", build_number=-1)

    def test_stamp_version_rewrites_the_build_number(self):
        header = self.tmp / "fnqaa_version.h"
        shutil.copyfile(fnqaa_meta.VERSION_HEADER, header)
        info = manual_release.stamp_version(header, 41)
        defines = fnqaa_meta.read_version_defines(header)
        self.assertEqual(defines["FNQAA_VERSION_TWEAK"], 41)
        self.assertEqual(defines["FNQAA_VERSION_STRING"], info["version_string"])
        self.assertTrue(str(defines["FNQAA_WINDOWS_FILE_VERSION"]).endswith(",41"))
        self.assertEqual(defines["FNQAA_TAG_PREFIX"], "v")


class ReleaseNotesTests(TempDir):
    def test_queue_is_published_whole_with_a_promoted_highlight(self):
        path = self.tmp / "CHANGELOG.md"
        path.write_text(QUEUE, encoding="utf-8")
        body = manual_release.changelog_release_body(path)
        highlights, rest = body.split("## All changes")
        self.assertIn("The first compatibility entry, wrapped over two lines.", highlights)
        self.assertIn("An audio entry.", highlights)
        self.assertIn("A second compatibility entry.", rest)
        self.assertNotIn("None yet", body)
        self.assertNotIn("old entry", body)

    def test_empty_queue_yields_no_body(self):
        path = self.tmp / "CHANGELOG.md"
        path.write_text(QUEUE, encoding="utf-8")
        changelog.clear_unreleased(path)
        self.assertEqual(manual_release.changelog_release_body(path), "")

    def test_tag_notes_win_over_version_notes(self):
        (self.tmp / "0.0.2.41.md").write_text("## Highlights\n\n- version\n", encoding="utf-8")
        self.assertEqual(manual_release.find_curated_release_notes(
            self.tmp, release_tag="0.0.2.41-20261126-3f80a8a1", version_string="0.0.2.41").name, "0.0.2.41.md")
        (self.tmp / "0.0.2.41-20261126-3f80a8a1.md").write_text("# FnQAA\n\n## Highlights\n\n- tag\n",
                                                                encoding="utf-8")
        path = manual_release.find_curated_release_notes(
            self.tmp, release_tag="0.0.2.41-20261126-3f80a8a1", version_string="0.0.2.41")
        self.assertEqual(path.name, "0.0.2.41-20261126-3f80a8a1.md")
        self.assertEqual(manual_release.read_curated_release_notes(path), "## Highlights\n\n- tag")

    def test_housekeeping_commits_are_left_out_of_fallback_highlights(self):
        commits = [
            {"subject": "Add package discovery", "author": "dev", "parents": "1"},
            {"subject": "Reset pending changelog after release", "author": "dev", "parents": "1"},
            {"subject": "Merge branch 'x'", "author": "dev", "parents": "2"},
            {"subject": "Tidy", "author": "github-actions[bot]", "parents": "1"},
        ]
        self.assertEqual(manual_release.fallback_highlights(commits), "- Add package discovery.")

    def test_workflow_commands_in_notes_are_neutralised(self):
        self.assertEqual(manual_release.sanitize_notes("::set-output x"), "\\::set-output x")


class PackagingTests(TempDir):
    def test_archive_names(self):
        ok = package_release.ARCHIVE_NAME.match
        self.assertTrue(ok("fnqaa-0.0.2-linux-x86_64.tar.gz"))
        self.assertTrue(ok("fnqaa-0.0.2.41-20261126-3f80a8a1-windows-x86_64.zip"))
        self.assertFalse(ok("fnqaa-0.0.2-windows-x86_64.tar.gz"))
        self.assertFalse(ok("fnqaa-../x-linux-x86_64.tar.gz"))

    def test_retail_and_recompiled_names_never_ship(self):
        bad = ["d/qaa_qagame.dll", "d/libqaa_ui.so", "d/qaa_guest_runtime.dll", "d/584108C8/000D0000/x",
               "d/setup/build/recomp/x", "d/ref/a", "d/default.xex", "d/qaa_default_xex_header.bin",
               "d/baseq3/pak0.bpk"]
        for name in bad:
            self.assertTrue(package_release.FORBIDDEN.search(name), name)
        for name in package_release.setup_kit():
            self.assertFalse(package_release.FORBIDDEN.search("d/setup/" + name), name)

    def test_checksums_and_manifest(self):
        archive = self.tmp / "fnqaa-0.0.2.41-20261126-3f80a8a1-linux-x86_64.tar.gz"
        archive.write_bytes(b"synthetic")
        sums, manifest = self.tmp / "SHA256SUMS.txt", self.tmp / "release-manifest.json"
        package_release.checksums([archive], sums, manifest, {"release_tag": "0.0.2.41-20261126-3f80a8a1"})
        self.assertIn(archive.name, sums.read_text())
        data = json.loads(manifest.read_text())
        self.assertEqual(data["release_tag"], "0.0.2.41-20261126-3f80a8a1")
        self.assertEqual(data["archives"][0]["platform"], "linux")
        self.assertEqual(data["archives"][0]["size"], 9)


def stfs_header(content_type=0x000D0000, title=0x584108C8):
    head = bytearray(0x1000)
    head[:4] = b"LIVE"
    head[0x344:0x348] = struct.pack(">I", content_type)
    head[0x360:0x364] = struct.pack(">I", title)
    return bytes(head)


class SetupDiscoveryTests(TempDir):
    NAME = "0ED9DD87783BD72BA56ED67ACD52BBFF1F47F08458"

    def put(self, *parts, data=None):
        path = self.tmp.joinpath(*parts)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(stfs_header() if data is None else data)
        return str(path)

    def test_the_layouts_users_have(self):
        for layout in (("584108C8", "000D0000", self.NAME),
                       ("Content", "0000000000000000", "584108C8", "000D0000", self.NAME),
                       (self.NAME,)):
            shutil.rmtree(self.tmp)
            self.tmp.mkdir()
            where = self.put(*layout)
            self.assertEqual(setup_env.find_package([str(self.tmp)]), (where, "package"), layout)

    def test_saves_and_other_titles_are_skipped(self):
        self.put("Content", "E0000ABCDEF00000", "584108C8", "00000001", "save", data=stfs_header(1))
        self.put("other", data=stfs_header(title=0x12345678))
        self.assertEqual(setup_env.find_package([str(self.tmp)]), (None, None))

    def test_the_xbla_zip_is_found_and_unpacked_once(self):
        zpath = self.tmp / "Quake Arena Arcade (World) (XBLA).zip"
        with zipfile.ZipFile(zpath, "w") as z:
            z.writestr("584108C8/000D0000/" + self.NAME, stfs_header() + b"\0" * 100)
        self.assertEqual(setup_env.find_package([str(self.tmp)]), (str(zpath), "zip"))
        dest = setup_env.unpack_zip(str(zpath), str(self.tmp / "release"))
        self.assertTrue(setup_env.is_package(dest))
        self.assertEqual(Path(dest).relative_to(self.tmp / "release").as_posix(), "584108C8/000D0000/" + self.NAME)
        self.assertEqual(setup_env.unpack_zip(str(zpath), str(self.tmp / "release")), dest)


if __name__ == "__main__":
    unittest.main()
