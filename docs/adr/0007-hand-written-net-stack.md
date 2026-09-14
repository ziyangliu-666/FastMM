# ADR-0007: Hand-written network stack over Boost.Asio / libwebsockets

Status: accepted (2026-09)

## Context

Asio adds a large dependency and an executor model that hides syscalls; we want explicit control over epoll, buffers and TLS I/O.

## Decision

epoll edge-triggered reactor, non-blocking TCP, OpenSSL memory BIOs, our own RFC 6455 and HTTP/1.1 clients. Streams are a concept.

## Consequences

More code to maintain, unit-tested in-process; no third-party network dependency.
