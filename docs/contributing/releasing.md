# Cutting a release

A release is a `v<version>` tag. Pushing the tag builds and publishes everything.

## 1. Set the version

`project(fastmm VERSION <x.y.z>)` in [`CMakeLists.txt`](../../CMakeLists.txt) is the only source. `pyproject.toml` reads it for both wheels. Two places repeat it by hand, and a version bump changes all three in one commit: the `live` extra in `pyproject.toml` pins `fastmm-engine-live==<x.y.z>` (`python/tests/test_packaging.py` fails when they disagree), and [`examples/quickstart/CMakeLists.txt`](../../examples/quickstart/CMakeLists.txt) fetches `GIT_TAG v<x.y.z>` (configuring FastMM with examples fails when they disagree). Run `python3 tools/doc_snippets.py` afterwards: the Quick start quotes that file.

What a version number promises: [Versions and compatibility](../reference/compatibility.md).

## 2. Write the CHANGELOG section

Rename `## [Unreleased]` to `## [<x.y.z>] - <YYYY-MM-DD>`. `release.yml` copies the section into the GitHub Release verbatim and appends the install lines and a compare link, so write it for someone scanning a release page:

- A minor release opens with one or two sentences on what it is about. A patch release has no lead.
- A section with more than 25 bullets gets a `Highlights:` list after the lead: three to six one-line items a user would upgrade for, each ending with the section and area it is detailed under, e.g. `(Added, venues)`.
- Sections, in this order and only when non-empty: `### Breaking changes`, `### Added`, `### Changed`, `### Removed`, `### Fixed`, `### Documentation`. Anything that stops an existing config key, file format, call form or install from working goes under Breaking changes. Within a section, bullets are grouped by area.
- One change per bullet, one line of at most 100 characters, prefixed with the area: `- venues: a warm standby started early in a rate-limit minute has its books before the handoff.` Say what the user sees now and stop: no "so that", "instead of", "which" or "because" clauses, no examples in parentheses, no story of how it was found. Keep config keys, flags, commands and API names in backticks. Under Breaking changes one extra clause may say what to do (`set "stay" for the old behaviour`).
- Leave out test-only, CI-only and internal changes.

## 3. Check it locally

```bash
cmake --build --preset release -j"$(nproc)" && ctest --preset release -j"$(nproc)"
./scripts/format.sh --check && ./scripts/docs-serve.sh --build
python3 -m build --sdist && python3 -m twine check dist/*
./scripts/package-release.sh --container --preset release --out dist && ./scripts/check-release.sh dist/*.tar.gz
```

## 4. Tag

```bash
TZ=UTC git tag -s v<x.y.z> -m "FastMM <x.y.z>"
git push origin v<x.y.z>
```

## 5. What the tag runs

| Workflow | Job | Result |
|---|---|---|
| `wheels.yml` | `sdist`, `wheels`, `live-wheels` | the sdist and the manylinux_2_28 wheels, each tested with its own test suite |
| `wheels.yml` | `publish` | uploads them to PyPI with the repository secret `PYPI_API_TOKEN`, in the `pypi` environment, only after the three build jobs pass |
| `release.yml` | `tarball` | `scripts/package-release.sh --container` (manylinux_2_28), `scripts/check-release.sh` on five distributions, then a GitHub Release with the tarball, its `.sha256` and the CHANGELOG section |
| `release.yml` | `image` | `deploy/docker/Dockerfile.production` pushed to `ghcr.io/ziyangliu-666/fastmm` as `<x.y.z>` and `latest` |

The `pypi` environment's deployment rules have to admit the tag ref, or `publish` stops before uploading. Both workflows also take a manual run (`workflow_dispatch`): `wheels.yml` with **publish** checked, `release.yml` with the tag name.

## 6. Check what was published

```bash
pip download --no-deps -d /tmp/rel "fastmm-engine==<x.y.z>" "fastmm-engine-live==<x.y.z>"
docker run --rm ghcr.io/ziyangliu-666/fastmm:<x.y.z> fastmm-live --version
gh release view v<x.y.z>
```

PyPI files cannot be replaced: a broken upload needs a new patch version. The wheels can be built and inspected before the tag by running `wheels.yml` by hand with **publish** unchecked.
