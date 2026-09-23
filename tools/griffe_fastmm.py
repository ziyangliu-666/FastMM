"""Griffe extension for the generated stub `python/fastmm/_core.pyi` (mkdocs.yml, mkdocstrings).

The stub's docstrings come from the C++ bindings through pybind11-stubgen
(docs/contributing/python-packages.md), so they are plain text, not Markdown. A configuration
section such as `[risk]` or `[[instruments]]` in one of them reads as a Markdown reference link,
which mkdocs-autorefs then reports as a missing cross-reference. Escaping the brackets keeps them
on the page as written and keeps the reference check meaningful for the hand-written pages.
"""
from __future__ import annotations

import re
from typing import Any

import griffe

STUB = "_core.pyi"
BRACKET = re.compile(r"(?<!\\)([\[\]])")


class EscapeStubBrackets(griffe.Extension):
    """Escapes `[` and `]` in the docstrings that come from the compiled core's stub."""

    def on_instance(self, *, obj: griffe.Object, **kwargs: Any) -> None:  # noqa: ARG002
        doc = obj.docstring
        if doc is None or doc.value is None:
            return
        path = getattr(obj, "filepath", None)
        if path is None or not str(path).endswith(STUB):
            return
        doc.value = BRACKET.sub(r"\\\1", doc.value)
