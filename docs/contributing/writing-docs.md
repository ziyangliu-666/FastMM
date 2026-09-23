# Writing docs

The Markdown under `docs/` is the source of both the site at <https://ziyangliu-666.github.io/FastMM/>
(MkDocs + Material, `mkdocs.yml`) and the pages GitHub renders, so relative links have to work in
both. Preview the site:

```bash
./scripts/docs-serve.sh          # http://127.0.0.1:8000, reloads on edit; --api adds the API reference
```

The script builds `build/docs-venv` from [`docs/requirements.txt`](../requirements.txt) the first
time and touches nothing else on the machine.

## Where a page goes

| Directory | Kind | A page |
|---|---|---|
| `docs/getting-started/` | tutorial | gets a new reader to a first result |
| `docs/tutorials/` | tutorial | teaches one path from start to finish, with no options |
| `docs/how-to/{strategies,venues,operations}/` | how-to | starts from a goal and lists the steps |
| `docs/reference/` | reference | describes one thing completely, with no narrative |
| `docs/explanation/` | explanation | gives background and reasons, with no steps |
| `docs/contributing/` | how-to | covers work on FastMM itself |
| `docs/adr/` | decision record | records one decision; not edited after acceptance except for amendments |

A new page gets an entry in the `nav` of [`mkdocs.yml`](../../mkdocs.yml) and a link from [`docs/README.md`](../README.md), which is the site's home page; the site build fails on a page that is missing from the nav. The glossary defines each term once. Python pages (`docs/python.md` and the Python reference) belong to the Python package.

## Style

1. Start with the first fact or step. No "This page explains ..." openings, no recap closings.
2. Keep a sentence only if deleting it would make a reader act wrongly, lose a fact or miss a link.
3. Do not restate the table, code, command output or generated help next to the sentence.
4. No reassurance ("normal", "harmless", "expected"): state the observable, the cause and when it matters.
5. Each fact has one home; other pages link to it. The risk warning lives on one page, [Go-live checklist](../how-to/operations/go-live-checklist.md); link to it.
6. Design reasons live in `docs/explanation/` and `docs/adr/`; other pages link to them in one clause.
7. Describe only what is implemented: no plans, ADR steps or task numbers in reader pages.
8. No bold except a glossary term at its definition or one warning per page; no bold run-in labels.
9. No emphasis words: simply, just, easily, fully, complete, note that; "exactly" and "every" only when verified.
10. No disclaimers or "what it is not" framing: no "has not been used with real money", no "at your own risk", no defence against questions nobody asked. Limits are facts with a file reference, on the page that owns them.
11. Second person and present tense in tutorials and how-tos; sentence-case headings; British spelling; no emojis.
12. Units on every number (bps, ms, s, ticks, base or quote currency, raw fixed-point integers).
13. Commands run from the repository root and copy as they are; code longer than three lines is a snippet; relative links only.
14. Write each paragraph and list item on one line; do not wrap prose at a column.

## Snippets

A code block that shows repository code is generated from the source by `tools/doc_snippets.py`:

````markdown
<!-- snippet: examples/cpp/tutorial/first_mm.hpp#on_book -->
```cpp
(the tool writes the region here)
```
````

The path is relative to the repository root; `#region` selects a region, and without it the whole file is shown. The source marks the region with comment lines, `// [start:on_book]` and `// [end:on_book]` in C++, `# --8<-- [start:name]` and `# --8<-- [end:name]` in shell, TOML, CMake and Python. The tool fills the block below each marker. Program output in `text` blocks may be shortened with `...`. Markers inside a code block, like the one above, are left alone.

```bash
python3 tools/doc_snippets.py
```

Code that the docs show should be compiled or run by a test: the examples and the tutorial have ctest labels `examples` and `tutorial`, `scripts/docs/tutorial.sh` holds the tutorial's shell steps, and `tests/docs/strategy_api_doc_test.cpp` holds the strategy API reference.

## Generated pages

| Page | Generated from | Tool |
|---|---|---|
| `docs/reference/cli.md` | each program's `--help` | `python3 tools/docs_cli_help.py --bin build/release/bin` |
| `docs/reference/configuration.md` | the key tables in `include/fastmm/config/schema.hpp` | `python3 tools/docs_config_ref.py` |

Only the regions between `<!-- BEGIN ... -->` and `<!-- END ... -->` are generated; edit the rest of the page by hand. Change a flag or a configuration key in the code (usage text or schema doc string), then run the tool.

## The API reference

Two pages are built from the code by the site, not committed:

| Page | Built from | By |
|---|---|---|
| `/api/cpp/` | the headers in [`docs/api/public-headers.txt`](../api/public-headers.txt) | Doxygen (`doxygen/Doxyfile.in`), through `tools/mkdocs_hooks.py` |
| `/api/python/` | the docstrings of `python/fastmm/` and the stub `_core.pyi` | mkdocstrings, from `docs/api/python.md` |

The headers document themselves with `//` comments; `tools/doxygen_filter.py` presents them to Doxygen as `///` and `///<` documentation, so a header needs no Doxygen markup. Adding a header to the manifest adds it to the reference. [`docs/api/cpp.md`](../api/cpp.md) is the hand-written index in front of the Doxygen output, grouped by subsystem; its links are checked against the generated pages on every build.

## Checks

CI runs these checks:

| Check | Command | CI job |
|---|---|---|
| snippets match their sources | `python3 tools/doc_snippets.py --check` | lint |
| relative links and anchors resolve | `python3 tools/docs_links.py --check` | lint |
| configuration reference is current | `python3 tools/docs_config_ref.py --check` | lint |
| command-line reference is current | `python3 tools/docs_cli_help.py --check --bin build/release/bin` | gcc-release |
| strategy API as documented, every public header compiles alone | `ctest --test-dir build/release -L docs` | all build jobs |
| quick start, examples and tutorial run | `ctest --test-dir build/release -L 'examples\|tutorial'` | all build jobs |
| every `configs/*.toml` loads without warnings | `ctest --test-dir build/release -L config` | all build jobs |
| the site builds with no broken link and every page in the nav | `./scripts/docs-serve.sh --build` | docs |

The tutorial script test (`tutorial.script`) runs the simulated exchange on a free port (`FASTMM_SIM_PORT=0`) and is not registered under sanitizers. The public header manifest is [`docs/api/public-headers.txt`](../api/public-headers.txt) ([Public API](../reference/public-api.md)).
