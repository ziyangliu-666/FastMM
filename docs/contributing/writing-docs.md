# Writing docs

How FastMM's documentation is organised, written and checked. The docs are Markdown rendered on
GitHub; there is no site generator.

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

A new page gets a link from [`docs/README.md`](../README.md). Python pages (`docs/python.md` and
the Python reference) belong to the Python package.

## Style

- Second person, present tense, sentence-case headings, short paragraphs, British spelling
  (normalise, behaviour), no emojis.
- Commands run from the repository root and can be copied as they are: `./build/release/bin/...`,
  or a variable defined once on the page.
- Units are always explicit: bps, ms, s, ticks, base or quote currency, raw fixed-point integers.
- No hand-copied code longer than three lines: use a snippet (below). Program output in `text`
  blocks may be shortened with `...`.
- A warning block (`> **Warning.**`) comes before any step that places orders on a venue.
- Relative links only; the glossary is the one definition of a term.

## Snippets

A code block that shows repository code is generated from the source by `tools/doc_snippets.py`:

````markdown
<!-- snippet: examples/cpp/tutorial/first_mm.hpp#on_book -->
```cpp
(the tool writes the region here)
```
````

The path is relative to the repository root; `#region` selects a region, and without it the whole
file is shown. The source marks the region with comment lines, `// [start:on_book]` and
`// [end:on_book]` in C++, `# --8<-- [start:name]` and `# --8<-- [end:name]` in shell, TOML,
CMake and Python. Write the marker line, run the tool, and it inserts or rewrites the fenced block
below the marker. Markers inside a code block, like the one above, are left alone.

```bash
python3 tools/doc_snippets.py
```

Code that the docs show should be compiled or run by a test: the examples and the tutorial have
ctest labels `examples` and `tutorial`, `scripts/docs/tutorial.sh` holds the tutorial's shell
steps, and `tests/docs/strategy_api_doc_test.cpp` holds the strategy API reference.

## Generated pages

| Page | Generated from | Tool |
|---|---|---|
| `docs/reference/cli.md` | each program's `--help` | `python3 tools/docs_cli_help.py --bin build/release/bin` |
| `docs/reference/configuration.md` | the key tables in `include/fastmm/config/schema.hpp` | `python3 tools/docs_config_ref.py` |

Only the regions between `<!-- BEGIN ... -->` and `<!-- END ... -->` are generated; edit the rest
of the page by hand. Change a flag or a configuration key in the code (usage text or schema doc
string), then run the tool.

## Checks

CI runs these; run them before you push a docs change.

| Check | Command | CI job |
|---|---|---|
| snippets match their sources | `python3 tools/doc_snippets.py --check` | lint |
| relative links and anchors resolve | `python3 tools/docs_links.py --check` | lint |
| configuration reference is current | `python3 tools/docs_config_ref.py --check` | lint |
| command-line reference is current | `python3 tools/docs_cli_help.py --check --bin build/release/bin` | gcc-release |
| strategy API as documented, every public header compiles alone | `ctest --test-dir build/release -L docs` | all build jobs |
| quick start, examples and tutorial run | `ctest --test-dir build/release -L 'examples\|tutorial'` | all build jobs |
| every `configs/*.toml` loads without warnings | `ctest --test-dir build/release -L config` | all build jobs |

The tutorial script test (`tutorial.script`) runs the simulated exchange on ports 9080 and 9443 and
is not registered under sanitizers. The public header manifest is
[`docs/api/public-headers.txt`](../api/public-headers.txt) ([Public API](../reference/public-api.md)).
