# ADR-0003: #pragma once instead of include guards

Status: accepted (2026-09)

## Context

Include guards are verbose and error-prone across a large header-heavy tree.

## Decision

Use `#pragma once` everywhere. FastMM builds on Linux with gcc and clang (CI: gcc 13 and clang 18), and both implement it.

## Consequences

Headers must not be copied to multiple paths (pragma once is path-based).
