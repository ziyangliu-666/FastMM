# ADR-0006: Static libraries with LTO

Status: accepted (2026-09)

## Context

Cross-TU inlining between compiled net/venue code and header-only engine code is worth more than shared-library ergonomics.

## Decision

All fastmm libraries are STATIC; Release presets enable IPO. Frame pointers are kept for profiling.

## Consequences

Binaries are self-contained apart from libssl/libz. Python builds force PIC.
