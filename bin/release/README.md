# bin/release

Cut releases only. Nothing in the build writes here: `make` always emits into
`bin/beta/`, and an artifact is copied in by hand at the moment a release is
made, so that "is this a release?" is answered by the directory it lives in
rather than by its filename.

| directory | contents | written by |
|---|---|---|
| `bin/beta/` | every build we make (Linux, Windows, `-san`, `-cov`) | `make -C src` |
| `bin/release/` | a cut release; empty until we are ready | a human, deliberately |
| `bin/old_versions/` | released **jalv2** versions (the historical line) | archive only |

jALv3 is beta. A versioned jALv3 artifact never goes into `bin/old_versions/`
— that directory is for released old versions, and `.gitignore` enforces it
(`bin/old_versions/jalv3*`).

## Before placing a release here

Stripping is a **size courtesy, not a gate** (2026-11-01: embedded build paths
are accepted in the published tree). A `-g` build is ~4.2/4.8 MB and carries
absolute paths in its DWARF; the stripped copies are ~950 kB:

```sh
cp bin/beta/jalv3-x86-64 bin/release/jalv3-x86-64
cp bin/beta/jalv3-win64.exe bin/release/jalv3-win64.exe
strip bin/release/jalv3-x86-64
x86_64-w64-mingw32-strip bin/release/jalv3-win64.exe
```
