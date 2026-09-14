# Contributing

1. `./scripts/bootstrap.sh --dev` configures `release` and `debug` and installs the pre-commit hooks.
2. Build and test: `cmake --workflow --preset debug`, or `cmake --build --preset release -j && ctest --preset release`.
3. Sanitizers: `cmake --workflow --preset asan` and `cmake --workflow --preset tsan`. On kernels with
   high ASLR entropy TSan aborts with "unexpected memory mapping"; run
   `setarch $(uname -m) -R ctest --preset tsan` (or `sudo sysctl -w vm.mmap_rnd_bits=28`).
4. Benchmarks: `./scripts/bench.sh --preset release-native --cpu 2` regenerates `bench/README.md`;
   `python3 tools/check_budgets.py bench/results/latest` compares the results with `bench/ci_budget.toml`.
5. Format: `./scripts/format.sh`; lint: `./scripts/tidy.sh`.
6. Commits follow Conventional Commits (`feat(core): ...`, `fix(net): ...`).
7. Hot-path code must not allocate or throw; cover it with a `NoAllocScope` test in
   `tests/hotpath/noalloc_test.cpp`.
8. Design decisions go in `docs/adr/` (about 15 lines).
9. Never construct `BookDeltaMsg` by value; see the comment on the struct in `core/messages.hpp`.
10. Docs follow [Writing docs](docs/contributing/writing-docs.md); run
    `python3 tools/doc_snippets.py --check` and `python3 tools/docs_links.py --check`.
