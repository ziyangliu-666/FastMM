# Contributing

1. `./scripts/bootstrap.sh --dev` (configures `release` + `debug`, installs pre-commit hooks).
2. Build & test: `cmake --workflow --preset debug` / `--preset asan` / `--preset tsan`.
3. Format: `./scripts/format.sh`; lint: `./scripts/tidy.sh`.
4. Commits follow Conventional Commits (`feat(core): ...`, `fix(net): ...`).
5. Hot-path code must stay allocation-free and exception-free; add a `NoAllocScope` test.
6. Design decisions go in `docs/adr/` (short, ~15 lines).
7. Sanitizers: `cmake --workflow --preset asan` and `--preset tsan`. On kernels with high ASLR
   entropy TSan aborts with "unexpected memory mapping". Run it with ASLR disabled:
   `setarch $(uname -m) -R ctest --preset tsan` (or `sudo sysctl -w vm.mmap_rnd_bits=28`).
8. Never construct `BookDeltaMsg` by value; see the comment on the struct in `core/messages.hpp`.
9. Docs: [Writing docs](docs/contributing/writing-docs.md) covers page kinds, snippets, generated
   references and the checks (`python3 tools/doc_snippets.py --check`, `tools/docs_links.py --check`).
