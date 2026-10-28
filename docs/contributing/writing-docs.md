# Writing docs

The Markdown under `docs/` is the source of both the site at <https://ziy.bio/FastMM/>
(MkDocs + Material, `mkdocs.yml`) and the pages GitHub renders, so relative links have to work in
both. Preview the site:

```bash
./scripts/docs-serve.sh          # http://127.0.0.1:8000, reloads on edit
```

The script creates `build/docs-venv` from [`docs/requirements.txt`](../requirements.txt) on first use; `--api` adds the API reference.

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

A new page gets an entry in the `nav` of [`mkdocs.yml`](../../mkdocs.yml); the site build fails on a page that is missing from the nav. [`docs/README.md`](../README.md), the home page, links only the five entry points. The glossary defines each term once. Python pages (`docs/python.md` and the Python reference) belong to the Python package.

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
13. Commands run from the repository root and copy as they are: no `$` prompt, in a `bash` block, with their output in a separate `text` block after it; code longer than three lines is a snippet; relative links only.
14. Write each paragraph and list item on one line; do not wrap prose at a column.
15. Write for the reader of the page, not for a reviewer: no qualifier that only protects the writer (a trailing parenthesis of requirements, "with no X", "only Y", "rather than Z").
16. A warning appears only if a reader following the page would hit the problem. Something learned while writing the page, that the page's reader cannot run into, stays out.
17. State a capability or leave it out; do not state it and take it back in the same sentence. Its limits belong on its reference page.
18. Edit by deleting first, then by moving the sentence to the page that owns it, and only then by rewriting it. Do not repair a sentence by adding a clause.
19. Short pages over complete ones: a page answers what its reader came for and stops. Over about 3,000 words, check whether it mixes page kinds before splitting it.
20. A paragraph holds one idea in at most four sentences or about 80 words; a sentence stays under 25 words with at most one parenthesis. The first sentence of each section and paragraph states its point.
21. Pick the form by the content: three or more like things with the same attributes go in a table; anything a reader copies goes in a code block with its real output; components and arrows go in a [diagram](#diagrams); something that changes over time goes in an animation; reasoning ("because", "so", "but") stays in prose. A sentence naming more than two or three code identifiers is reference material and belongs in a table.
22. Write for the reader, not about the work: no "we measured", "was built", "three runs on <date>", internal class names or header paths in narrative. A number page gives the result, one sentence on what was measured and on what hardware, and links to its method; how a measurement or a feature was made goes to its method page, the CHANGELOG or an ADR.
23. Check a page rendered (`./scripts/docs-serve.sh`): scroll it screen by screen and read only the first sentence of each paragraph. A screen of unbroken text, or first sentences that do not tell the story, means the page needs a table, a diagram, steps or a cut.

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

A `text` block of a backtest's output is preceded by the command that prints it, `<!-- output: fastmm-backtest --config configs/backtest-example.toml --data synthetic --out - -->` (the program, then its arguments). The test `docs.reference_output` runs each one and fails when a line of the block, `...` matching anything, is not in the output; after a change that moves the numbers, rerun the command and paste them.

## Diagrams

A diagram is a hand-written SVG in `docs/assets/diagrams/`, drawn like the animations in `docs/assets/anim/`: black header bars on threads, rounded capsules for rings, the building for a venue, red for the one or two things to see first, a legend at the bottom. It holds at most 9 boxes and 12 arrows, each arrow labelled with what moves, and every label is at least 13 px on a canvas 720 px wide. `<name>.svg` is the source; `python3 tools/docs_diagrams.py` writes `<name>-dark.svg` from it. A page shows both:

```markdown
![what it shows](../assets/diagrams/gateway.svg#only-light){ .fastmm-diagram }
![what it shows](../assets/diagrams/gateway-dark.svg#only-dark){ .fastmm-diagram }
```

## Generated pages

| Page | Generated from | Tool |
|---|---|---|
| `docs/reference/cli.md` | each program's `--help` | `python3 tools/docs_cli_help.py --bin build/release/bin` |
| `docs/reference/configuration.md` | the key tables in `include/fastmm/config/schema.hpp` and each connector's `src/venues/<venue>/*_registration.cpp` | `python3 tools/docs_config_ref.py` |

Only the regions between `<!-- BEGIN ... -->` and `<!-- END ... -->` are generated; edit the rest of the page by hand. Change a flag or a configuration key in the code (usage text or doc string), then run the tool.

## The API reference

Two pages are built from the code by the site, not committed:

| Page | Built from | By |
|---|---|---|
| `/api/cpp/` | the headers in [`docs/api/public-headers.txt`](../api/public-headers.txt) | Doxygen (`doxygen/Doxyfile.in`), through `tools/mkdocs_hooks.py` |
| `/api/python/` | the docstrings of `python/fastmm/` and the stub `_core.pyi` | mkdocstrings, from `docs/api/python.md` |

The headers document themselves with `//` comments; `tools/doxygen_filter.py` presents them to Doxygen as `///` and `///<` documentation, so a header needs no Doxygen markup. Adding a header to the manifest adds it to the reference. [`docs/api/cpp.md`](../api/cpp.md) is the hand-written index in front of the Doxygen output, grouped by subsystem; its links are checked against the generated pages on every build.

## Checks

| Check | Command | CI job |
|---|---|---|
| snippets match their sources | `python3 tools/doc_snippets.py --check` | lint |
| relative links and anchors resolve | `python3 tools/docs_links.py --check` | lint |
| each diagram's dark variant is current | `python3 tools/docs_diagrams.py --check` | lint |
| configuration reference is current | `python3 tools/docs_config_ref.py --check` | lint |
| command-line reference is current | `python3 tools/docs_cli_help.py --check --bin build/release/bin` | gcc unit |
| strategy API as documented, every public header compiles alone, quoted outputs and versions are current | `ctest --test-dir build/release -L docs` | all build jobs |
| quick start, examples and tutorial run | `ctest --test-dir build/release -L 'examples\|tutorial'` | all build jobs |
| every `configs/*.toml` loads without warnings | `ctest --test-dir build/release -L config` | all build jobs |
| the site builds with no broken link and every page in the nav | `./scripts/docs-serve.sh --build` | docs |

The tutorial script test (`tutorial.script`) runs the simulated exchange on a free port (`FASTMM_SIM_PORT=0`) and is not registered under sanitizers. The public header manifest is [`docs/api/public-headers.txt`](../api/public-headers.txt) ([Public API](../reference/public-api.md)).
