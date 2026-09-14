# ADR-0004: doctest over GoogleTest

Status: accepted (2026-09)

## Context

The codebase is template-heavy; test compile time matters across four CI configurations.

## Decision

doctest: single header, lighter to compile than gtest (not measured here), `SUBCASE` fits state-machine tests, `doctest_discover_tests` gives per-case ctest entries.

## Consequences

No gmock; we test against reference models (naive `std::map` books) rather than mocks.
