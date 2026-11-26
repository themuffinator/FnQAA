# Curated FnQAA release notes

Use this directory only when a release needs reviewed prose beyond the
`Unreleased` queue in [`../CHANGELOG.md`](../CHANGELOG.md). Name the file
either `<release-tag>.md` (preferred) or `<version>.md`, for example
`0.0.2.41-20261126-3f80a8a1.md` or `0.0.2.41.md`. `python
scripts/manual_release.py summary` prints the next release's tag and version.

The release workflow uses the tag-specific file first, then the version file,
and the changelog queue only when neither exists. A top-level title is
optional and is removed from the published body. Start the content with:

```markdown
## Highlights

- The player-visible change and what it means for retail behaviour.
- Another concise change worth testing.

## Known limitations

- A concrete limitation, its scope, and the safe fallback where there is one.
```

The release notes always go on with the getting-started steps, the build
details and the included commits. Keep claims grounded in verified Quake
Arena Arcade behaviour (docs/FORMATS.md marks what is verified).
