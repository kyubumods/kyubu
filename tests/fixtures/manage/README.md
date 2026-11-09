# fixtures/manage

The shared pieces of task 3.2 of add-mod-manager (listing every package, the D5 package hash):
`tests/unit/test_mod_management.cpp` copies these packages into a temporary game folder, and `kcat`
(that change, section 5) must hash with the same rule and the same golden values.

## The packages

| Folder     | id             | What it is in the test |
|------------|----------------|------------------------|
| `canonical/` | `mm_loaded`   | the calling mod (full trust, `manage_mods = true`), the loaded mod, and the folder-hash vector |
| `disabled/`  | `mm_disabled` | disabled: `[mods.mm_disabled] enabled = false` in `kyubu\config.toml` |
| `failed/`    | `mm_failed`   | `version = "not a version"`, so the manifest does not parse |
| `restricted/`| `mm_restricted`| restricted trust with `manage_mods = true`: still denied |
| `zipped/`    | `mm_zipped`   | zipped into `mods\mm_zipped.zip` at test time; declares a dependency on `mm_loaded` |

The test zips `zipped/` itself (a zip's timestamps only exist at creation), so only the folder
hash has a golden vector here.

## The folder-hash rule (design.md D5, "Package hash")

For each file of the folder, relative path with `/`, its size in bytes and its own SHA-256, sorted
by path (byte order of the relative path), one line each:

```
<path> <size> <sha256-of-the-file>\n
```

The package hash is the SHA-256 of that listing text (ASCII, LF endings, every line ends with
`\n`, nothing else).

`canonical/` is the folder-hash vector:

- the per-file lines (path with `/`, size, SHA-256, sorted by path) and the package digest are
  kept in `golden.txt` under `listing:` and `folder:`;
- the test builds the listing from its own constants, hashes it, and compares both against
  `golden.txt` and against `mods_package_hash`.
