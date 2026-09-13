# ADR-0003: #pragma once instead of include guards

Status: accepted (2026-09)

## Context

Include guards are verbose and error-prone across a large header-heavy tree.

## Decision

Use `#pragma once` everywhere; all supported compilers (gcc 13+, clang 16+, msvc) implement it.

## Consequences

Headers must not be copied to multiple paths (pragma once is path-based).
