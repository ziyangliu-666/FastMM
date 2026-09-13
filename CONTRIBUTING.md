# Contributing

1. `./scripts/bootstrap.sh --dev` (configures `release` + `debug`, installs pre-commit hooks).
2. Build & test: `cmake --workflow --preset debug` / `--preset asan` / `--preset tsan`.
3. Format: `./scripts/format.sh`; lint: `./scripts/tidy.sh`.
4. Commits follow Conventional Commits (`feat(core): ...`, `fix(net): ...`).
5. Hot-path code must stay allocation-free and exception-free; add a `NoAllocScope` test.
6. Design decisions go in `docs/adr/` (short, ~15 lines).
