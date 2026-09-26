# Versions and compatibility

One number decides them all: `project(fastmm VERSION)` in [`CMakeLists.txt`](../../CMakeLists.txt). The generated `fastmm/version.hpp` (`FASTMM_VERSION_STRING`, `fastmm::kVersionString`, `fastmm-live --version`), both wheels (`fastmm.__version__`, `fastmm_live.__version__`), the release tarball and the container image read it; nothing else declares a version. A release is the tag `v<version>`, and [CHANGELOG.md](../../CHANGELOG.md) has one section per release.

Versions are `0.MINOR.PATCH`. A minor release may change any surface in the table below; a patch release changes none of them and only fixes behaviour. Entries in the CHANGELOG that break a surface start with the surface they break.

## What a 0.x minor release may change

| Surface | Across a minor release | Version today |
|---|---|---|
| Configuration keys | keys are added, renamed and removed. An unknown key is a warning with its line number, not an error, so a renamed key leaves the new one at its default ([Configuration](configuration.md)) | no schema version |
| `.fmj` journal | the format version rises when the layout changes. A reader opens every version from 1 up to its own; a newer journal is refused ([Journal format](journal-format.md)) | 3, readers open 1 to 3 |
| Status file | the version rises with every layout change, with no compatibility in either direction: `fastmm-top` refuses a file of another version (`--once` exits 3), so `fastmm-top`, `fastmm-live` and `fastmm-gateway` come from the same build ([Status file](status-file.md)) | 9 |
| Store | each schema change is a migration; an older store is migrated when opened, a newer one is refused ([Storage](storage.md)) | 4 |
| Gateway protocol | `fastmm-live --gateway` and `fastmm-gateway` must have the same version; an attach of another version is refused | 5 |
| Hot-hook ABI | `FASTMM_HOT_ABI_VERSION` (`include/fastmm/strategies/hot_abi.h`) rises when the structs a hot hook sees change. `fastmm._hot.abi` checks the layouts at import and raises `ImportError` on a mismatch | 1 |
| C++ API | strategy hooks, registration, the context methods and the header tiers change; a hook with the wrong signature fails the build naming the expected one ([Public API](public-api.md)) | — |
| Python API | names in `fastmm.__all__` are added and changed; a change that breaks a call form is a CHANGELOG entry | — |
| Command lines | flags are added; a removed flag exits 2 ([Command lines](cli.md)) | — |
| Exit codes | codes are added for new outcomes; an existing code keeps its meaning ([Errors and exit codes](errors.md)) | — |

## Upgrading

- `fastmm-engine` and `fastmm-engine-live` have to be the same version. `pip install "fastmm-engine[live]"` pins it, and `import fastmm_live` raises `ImportError` when the two disagree.
- Upgrade `fastmm-top` and `fastmm-gateway` together with `fastmm-live`, from the same tarball or image.
- Check the release's CHANGELOG section for renamed configuration keys before starting a session with an old config: unknown keys are reported, not refused.
- A replay that has to match a recorded session needs the binary that wrote the journal, not only a compatible reader ([Determinism](../explanation/determinism.md)).
