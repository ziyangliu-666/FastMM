"""What a live session hands the engine and records in its journal about a hot strategy class.

param_fields(spec) is the parameter layout the engine writes publishes into (HotStrategy's parameter
blocks and the journal's parameter table). session_meta(cls) is the journal's strategy
metadata: the class as ``module:qualname``, a hash of the hot-hook source and the package versions,
so a replay can tell whether it runs the same code.
"""

from __future__ import annotations

import hashlib
import inspect
import marshal
import platform
import textwrap
import types as pytypes
from typing import Any, Dict, List, Mapping

from .decl import HotSpec


def param_fields(spec: HotSpec) -> List[Dict[str, Any]]:
    """Each parameter in declaration order with its offsets in the ``self`` record."""
    dtype, _ = spec.record_dtype()
    out: List[Dict[str, Any]] = []
    for name, p in spec.params.items():
        is_bool = p.type is bool
        out.append({
            "name": name,
            "type": p.type_name,
            "offset": dtype.fields[name][1],
            "raw_offset": dtype.fields[name + "_raw"][1] if p.type is float else -1,
            "min": None if is_bool else p.min,
            "max": None if is_bool else p.max,
            "default": float(p.default),
            "doc": p.doc,
        })
    return out


def _source(fn: Any) -> bytes:
    try:
        return textwrap.dedent(inspect.getsource(fn)).encode()
    except (OSError, TypeError):  # no source file (exec, REPL)
        return marshal.dumps(fn.__code__)


_PLAIN = (bool, int, float, complex, str, bytes, type(None))


def _reachable(fn: Any) -> tuple:
    """(helpers, constants) of `fn`: the Python functions of the numba.njit functions it names in
    its globals or closure, directly or as an attribute of a module global (``lib.helper``), and the
    plain values (numbers, strings, tuples of them) of the globals it reads, which Numba freezes
    into the machine code."""
    names = set()
    codes = [fn.__code__]
    while codes:
        code = codes.pop()
        names.update(code.co_names)
        codes.extend(c for c in code.co_consts if isinstance(c, pytypes.CodeType))
    values = []
    constants: Dict[str, str] = {}
    for n in sorted(names):
        if n not in fn.__globals__:
            continue
        v = fn.__globals__[n]
        values.append(v)
        if isinstance(v, pytypes.ModuleType):
            for attr in sorted(names):
                w = getattr(v, attr, None)
                if w is not None:
                    values.append(w)
                    if isinstance(w, _PLAIN):
                        constants[f"{n}.{attr}"] = repr(w)
        elif isinstance(v, _PLAIN) or (
                isinstance(v, tuple) and all(isinstance(x, _PLAIN) for x in v)):
            constants[n] = repr(v)
    for cell in fn.__closure__ or ():
        try:
            values.append(cell.cell_contents)
        except ValueError:  # an empty cell
            continue
    helpers = [v.py_func for v in values
               if isinstance(getattr(v, "py_func", None), pytypes.FunctionType)]
    return helpers, constants


def hot_source_sha256(spec: HotSpec) -> str:
    """SHA-256 of the source of every hot hook and of every numba.njit function they reach, and of
    the plain global values they read (compile-time constants in Numba)."""
    hooks = {**spec.hooks, **spec.timers}
    helpers: Dict[str, Any] = {}
    constants: Dict[str, str] = {}
    pending = [h.fn for h in hooks.values()]
    while pending:
        fn = pending.pop()
        found, values = _reachable(fn)
        for name, value in values.items():
            constants[f"{fn.__module__}:{name}"] = value
        for helper in found:
            key = f"{helper.__module__}:{helper.__qualname__}"
            if key not in helpers:
                helpers[key] = helper
                pending.append(helper)
    digest = hashlib.sha256()
    for name in sorted(hooks):
        digest.update(f"hook {name}\n".encode())
        digest.update(_source(hooks[name].fn))
    for key in sorted(helpers):
        digest.update(f"helper {key}\n".encode())
        digest.update(_source(helpers[key]))
    for key in sorted(constants):
        digest.update(f"constant {key}={constants[key]}\n".encode())
    return digest.hexdigest()


def session_meta(cls: type) -> Dict[str, str]:
    """The journal's strategy metadata of a class with hot hooks."""
    import llvmlite
    import numba

    from .. import _core

    return {
        "class": f"{cls.__module__}:{cls.__qualname__}",
        "hot_source_sha256": hot_source_sha256(cls._fastmm_hot),  # type: ignore[attr-defined]
        "fastmm": _core.__version__,
        "numba": numba.__version__,
        "llvmlite": llvmlite.__version__,
        "python": platform.python_version(),
    }


def format_meta(meta: Mapping[str, str]) -> str:
    """``key=value`` lines."""
    lines = []
    for key, value in meta.items():
        if not key or "=" in key or "\n" in key or "\n" in value:
            raise ValueError(f"fastmm: journal metadata {key!r}={value!r} is not a key=value line")
        lines.append(f"{key}={value}\n")
    return "".join(lines)


def parse_meta(text: str) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for line in text.splitlines():
        key, sep, value = line.partition("=")
        if sep:
            out[key] = value
    return out
