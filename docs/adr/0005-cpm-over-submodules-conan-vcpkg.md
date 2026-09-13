# ADR-0005: CPM.cmake over submodules, Conan or vcpkg

Status: accepted (2026-09)

## Context

Users should clone, configure and build with only a compiler, CMake and OpenSSL.

## Decision

Dependencies are fetched at configure time by the vendored `cmake/CPM.cmake` with pinned tags and `SYSTEM YES`; `CPM_SOURCE_CACHE` makes repeat configures offline.

## Consequences

First configure needs network. Version pins live in one file (`cmake/Dependencies.cmake`).
