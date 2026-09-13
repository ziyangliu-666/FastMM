# ADR-0009: CRTP strategies over virtual interfaces

Status: accepted (2026-09)

## Context

A virtual call per book update is measurable at the nanosecond budgets we target, and it blocks inlining of the quote logic.

## Decision

`Engine<Strategy, Clock, Transport, Feed>` is a template; hooks are detected with `requires`. A thin registry instantiates the template per strategy name for runtime selection.

## Consequences

Each (strategy, transport) pair is a separate instantiation compiled in its own TU to bound build time.
