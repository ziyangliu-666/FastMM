# Cutting a release

A release is a `v<version>` tag. Pushing the tag builds and publishes everything.

## 1. Set the version

`project(fastmm VERSION <x.y.z>)` in [`CMakeLists.txt`](../../CMakeLists.txt) is the only source. `pyproject.toml` reads it for both wheels, and its `live` extra pins `fastmm-engine-live==<x.y.z>` by hand: bump both, in one commit. `python/tests/test_packaging.py` fails when they disagree.

What a version number promises: [Versions and compatibility](../reference/compatibility.md).

## 2. Write the CHANGELOG section

Rename `## [Unreleased]` to `## [<x.y.z>] - <YYYY-MM-DD>` and group it into one `### Added`, `### Changed`, `### Removed`, `### Fixed` and `### Documentation`. Every entry that changes a configuration key, a file format or a call form starts with what it breaks; `release.yml` copies the section into the GitHub Release notes verbatim.

## 3. Check it locally

```bash
cmake --build --preset release -j"$(nproc)" && ctest --preset release -j"$(nproc)"
./scripts/format.sh --check && ./scripts/docs-serve.sh --build
python3 -m build --sdist && python3 -m twine check dist/*
./scripts/package-release.sh --preset release --out dist
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
| `release.yml` | `tarball` | `scripts/package-release.sh`, then a GitHub Release with the tarball, its `.sha256` and the CHANGELOG section |
| `release.yml` | `image` | `docker/Dockerfile.production` pushed to `ghcr.io/ziyangliu-666/fastmm` as `<x.y.z>` and `latest` |

The `pypi` environment's deployment rules have to admit the tag ref, or `publish` stops before uploading. Both workflows also take a manual run (`workflow_dispatch`): `wheels.yml` with **publish** checked, `release.yml` with the tag name.

## 6. Check what was published

```bash
pip download --no-deps -d /tmp/rel "fastmm-engine==<x.y.z>" "fastmm-engine-live==<x.y.z>"
docker run --rm ghcr.io/ziyangliu-666/fastmm:<x.y.z> fastmm-live --version
gh release view v<x.y.z>
```

PyPI files cannot be replaced: a broken upload needs a new patch version. The wheels can be built and inspected before the tag by running `wheels.yml` by hand with **publish** unchecked.
