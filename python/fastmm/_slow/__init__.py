"""Slow methods of Python strategies (ADR-0013, section 1): @fastmm.every, the context slow methods
get, publishes, the runner and fastmm.replay. decl.py imports nothing heavy; the rest loads when a
strategy with slow methods runs."""
