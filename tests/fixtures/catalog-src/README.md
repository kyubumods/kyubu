# fixtures/catalog-src

Sources of the test catalog `tests/fixtures/catalog/` (add-mod-manager task 4.3). Not served; kept so the catalog can be rebuilt.

- `mods/`: `cat_lib 1.0.0`, `cat_alpha 1.0.0` and `1.1.0` (both need `cat_lib ^1.0`), `cat_beta 1.0.0`.
- `pack.toml`: `cat_pack`, which pins the older `cat_alpha 1.0.0` and `cat_beta`; kcat added `cat_lib 1.0.0`.
- `test.key`: a throwaway Ed25519 seed. It signs only this fixture. Public key (hex):
  `e3f0804338a7c54088b550c75fb0303a0777e194ccc43aee20a556eb7b535a0f`.

Rebuild: `kcat --catalog tests/fixtures/catalog --key tests/fixtures/catalog-src/test.key --yes add <mod folder>` for each
mod, then `pack pack.toml`, `sign`, `check`. Zips are reproducible, so only `generated` and `published` change.
