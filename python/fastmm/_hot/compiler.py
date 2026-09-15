"""Compilation of hot hooks and the backtest run.

Per hook::

    user = numba.njit(boundscheck=True, cache=...)(hook)
    def entry(self_p, ctx_p, book_p):              # generated, nopython
        self, ctx, book = carray(self_p, 1)[0], ...   # records over engine-owned memory
        try:
            user(self, ctx, book)
        except Exception:
            return EXCEPTION                        # Numba's C wrapper never sees an exception
        return ctx.status
    numba.cfunc(int32(CPointer(self), CPointer(ctx), CPointer(book)))(entry).address

The ctx methods are Numba overloads on the ctx record that write intents into the struct. Before a
hook is accepted, the LLVM IR of every compiled function except Numba's C wrapper is scanned for
calls outside an allowlist. Only the user function uses Numba's cache (the generated wrapper is a
closure, which Numba cannot cache), so the IR is available on every run.
"""

from __future__ import annotations

import contextlib
import hashlib
import os
import re
import time
import types as pytypes
from pathlib import Path
from typing import Any, Dict, Iterator, List, Mapping, Optional, Tuple

import llvmlite
import llvmlite.binding as llvm
import numba
from numba import carray, cfunc, njit, types
from numba.core import config as numba_config
from numba.core.errors import NumbaError, TypingError, UnsupportedBytecodeError
from numba.extending import overload_method

from .. import _core
from . import abi
from .decl import HotCompileError, HotSpec

CTX_TYPE = numba.from_dtype(abi.CTX_DTYPE)
BOOK_TYPE = numba.from_dtype(abi.BOOK_DTYPE)

# boundscheck: an index outside a book or quote array raises IndexError, which the wrapper turns
# into FASTMM_HOT_EXCEPTION. Python's error model: division by zero raises ZeroDivisionError.
JIT_OPTIONS: Dict[str, Any] = {"boundscheck": True, "error_model": "python"}

_EXCEPTION = abi.EXCEPTION
_FAILED = abi.FAILED
_QUOTE = abi.ACTION_QUOTE
_PULL = abi.ACTION_PULL
_UNCROSS = abi.FLAG_UNCROSS
_KEEP_PASSIVE = abi.FLAG_KEEP_PASSIVE
_LEVELS = abi.QUOTE_LEVELS


# ---- ctx methods -----------------------------------------------------------------------------------

def _is_ctx(t: Any) -> bool:
    return isinstance(t, types.Record) and t.dtype == abi.CTX_DTYPE


def _numbers(method: str, *args: Any) -> None:
    for a in args:
        if not isinstance(a, (types.Integer, types.Float, types.Boolean)):
            raise TypingError(f"fastmm: ctx.{method} takes numbers, got {a}")


def _ints(method: str, *args: Any) -> None:
    for a in args:
        if not isinstance(a, (types.Integer, types.Boolean)):
            raise TypingError(f"fastmm: ctx.{method} takes int raw values (1e-8 scale), got {a}; "
                              f"use ctx.{method[:-4]} for floats")


@overload_method(types.Record, "quote")
def _quote(ctx, bid_px, ask_px, qty):
    if not _is_ctx(ctx):
        return None
    _numbers("quote", bid_px, ask_px, qty)

    def impl(ctx, bid_px, ask_px, qty):  # type: ignore[no-untyped-def]
        ctx.bid_px[0] = bid_px
        ctx.bid_qty[0] = qty
        ctx.bid_is_raw[0] = 0
        ctx.ask_px[0] = ask_px
        ctx.ask_qty[0] = qty
        ctx.ask_is_raw[0] = 0
        ctx.n_bids = 1
        ctx.n_asks = 1
        ctx.action = _QUOTE
    return impl


@overload_method(types.Record, "quote_raw")
def _quote_raw(ctx, bid_px, ask_px, qty):
    if not _is_ctx(ctx):
        return None
    _ints("quote_raw", bid_px, ask_px, qty)

    def impl(ctx, bid_px, ask_px, qty):  # type: ignore[no-untyped-def]
        ctx.bid_px_raw[0] = bid_px
        ctx.bid_qty_raw[0] = qty
        ctx.bid_is_raw[0] = 1
        ctx.ask_px_raw[0] = ask_px
        ctx.ask_qty_raw[0] = qty
        ctx.ask_is_raw[0] = 1
        ctx.n_bids = 1
        ctx.n_asks = 1
        ctx.action = _QUOTE
    return impl


@overload_method(types.Record, "bid")
def _bid(ctx, px, qty):
    if not _is_ctx(ctx):
        return None
    _numbers("bid", px, qty)

    def impl(ctx, px, qty):  # type: ignore[no-untyped-def]
        n = ctx.n_bids
        if n < _LEVELS:
            ctx.bid_px[n] = px
            ctx.bid_qty[n] = qty
            ctx.bid_is_raw[n] = 0
            ctx.n_bids = n + 1
        ctx.action = _QUOTE
    return impl


@overload_method(types.Record, "ask")
def _ask(ctx, px, qty):
    if not _is_ctx(ctx):
        return None
    _numbers("ask", px, qty)

    def impl(ctx, px, qty):  # type: ignore[no-untyped-def]
        n = ctx.n_asks
        if n < _LEVELS:
            ctx.ask_px[n] = px
            ctx.ask_qty[n] = qty
            ctx.ask_is_raw[n] = 0
            ctx.n_asks = n + 1
        ctx.action = _QUOTE
    return impl


@overload_method(types.Record, "bid_raw")
def _bid_raw(ctx, px, qty):
    if not _is_ctx(ctx):
        return None
    _ints("bid_raw", px, qty)

    def impl(ctx, px, qty):  # type: ignore[no-untyped-def]
        n = ctx.n_bids
        if n < _LEVELS:
            ctx.bid_px_raw[n] = px
            ctx.bid_qty_raw[n] = qty
            ctx.bid_is_raw[n] = 1
            ctx.n_bids = n + 1
        ctx.action = _QUOTE
    return impl


@overload_method(types.Record, "ask_raw")
def _ask_raw(ctx, px, qty):
    if not _is_ctx(ctx):
        return None
    _ints("ask_raw", px, qty)

    def impl(ctx, px, qty):  # type: ignore[no-untyped-def]
        n = ctx.n_asks
        if n < _LEVELS:
            ctx.ask_px_raw[n] = px
            ctx.ask_qty_raw[n] = qty
            ctx.ask_is_raw[n] = 1
            ctx.n_asks = n + 1
        ctx.action = _QUOTE
    return impl


@overload_method(types.Record, "clear")
def _clear(ctx):
    if not _is_ctx(ctx):
        return None

    def impl(ctx):  # type: ignore[no-untyped-def]
        ctx.n_bids = 0
        ctx.n_asks = 0
        ctx.action = _QUOTE
    return impl


@overload_method(types.Record, "pull")
def _pull(ctx):
    if not _is_ctx(ctx):
        return None

    def impl(ctx):  # type: ignore[no-untyped-def]
        ctx.n_bids = 0
        ctx.n_asks = 0
        ctx.action = _PULL
    return impl


@overload_method(types.Record, "uncross")
def _uncross(ctx):
    if not _is_ctx(ctx):
        return None

    def impl(ctx):  # type: ignore[no-untyped-def]
        ctx.flags = ctx.flags | _UNCROSS
    return impl


@overload_method(types.Record, "keep_passive")
def _keep_passive(ctx):
    if not _is_ctx(ctx):
        return None

    def impl(ctx):  # type: ignore[no-untyped-def]
        ctx.flags = ctx.flags | _KEEP_PASSIVE
    return impl


@overload_method(types.Record, "fail")
def _fail(ctx, code):
    if not _is_ctx(ctx):
        return None
    if not isinstance(code, types.Integer):
        raise TypingError(f"fastmm: ctx.fail takes an int code, got {code}")

    def impl(ctx, code):  # type: ignore[no-untyped-def]
        ctx.status = _FAILED
        ctx.fail_code = code
    return impl


# ---- IR check ----------------------------------------------------------------------------------------

_LIBM = frozenset(
    name + suffix
    for name in (
        "acos acosh asin asinh atan atan2 atanh cbrt ceil copysign cos cosh erf erfc exp exp2 "
        "expm1 fabs fdim floor fmax fmin fmod frexp hypot ldexp lgamma log log10 log1p log2 logb "
        "modf nearbyint nextafter pow remainder rint round scalbn sin sinh sqrt tan tanh tgamma "
        "trunc"
    ).split()
    for suffix in ("", "f")
)
_ALLOWED = frozenset({"NRT_MemInfo_call_dtor"})
_DEFINE_RE = re.compile(r'^define\b[^\n@]*@"?([^"\s(]+)"?\(', re.M)
_CALL_RE = re.compile(r'\b(?:call|invoke)\b[^\n@]*@"?([^"\s(]+)"?\(')


def disallowed_calls(ir: str) -> List[str]:
    """External functions called by any function in `ir` other than Numba's C wrappers (`cfunc.*`)
    that are not LLVM intrinsics, libm or NRT_MemInfo_call_dtor."""
    defined = set(_DEFINE_RE.findall(ir))
    bad = set()
    for chunk in re.split(r"^(?=define\b)", ir, flags=re.M):
        m = _DEFINE_RE.match(chunk)
        if m is None or m.group(1).startswith("cfunc."):
            continue
        body = chunk.split("\n}\n", 1)[0]
        for callee in _CALL_RE.findall(body):
            if callee in defined or callee.startswith("llvm.") or callee in _LIBM:
                continue
            if callee not in _ALLOWED:
                bad.add(callee)
    return sorted(bad)


def _explain(symbol: str) -> str:
    if symbol.startswith("NRT_"):
        return "memory allocation (arrays, lists, dicts, strings)"
    if symbol.startswith(("Py", "_Py", "numba_gil")):
        return "the Python C API (print, Python objects)"
    return "a function outside the allowlist"


# ---- cache -------------------------------------------------------------------------------------------

def cache_dir() -> Path:
    """The Numba cache directory for this fastmm version, ABI, FastMM compiler sources, numba
    version and CPU. $FASTMM_CACHE_DIR overrides the base directory (default
    $XDG_CACHE_HOME/fastmm or ~/.cache/fastmm)."""
    base = os.environ.get("FASTMM_CACHE_DIR")
    if not base:
        xdg = os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache")
        base = os.path.join(xdg, "fastmm")
    here = Path(__file__).resolve().parent
    sources = hashlib.sha256()
    for f in (here / "compiler.py", here / "abi.py", here.parent / "fx.py"):
        sources.update(f.read_bytes())
    cpu = llvm.get_host_cpu_name()
    features = hashlib.sha256(llvm.get_host_cpu_features().flatten().encode()).hexdigest()[:8]
    key = (f"fastmm-{_core.__version__}-abi{abi.VERSION}-{abi.ABI_HASH}-src{sources.hexdigest()[:12]}"
           f"-numba{numba.__version__}-llvmlite{llvmlite.__version__}-{cpu}-{features}")
    return Path(base) / "numba" / key


@contextlib.contextmanager
def _compile_settings(cache: bool, boundscheck: bool) -> Iterator[None]:
    """Numba reads CACHE_DIR when a dispatcher is created and BOUNDSCHECK when a function is lowered,
    so helpers compiled on demand by a hook get bounds checks too."""
    old_dir, old_bc = numba_config.CACHE_DIR, numba_config.BOUNDSCHECK
    try:
        if cache:
            numba_config.CACHE_DIR = str(cache_dir())
        numba_config.BOUNDSCHECK = 1 if boundscheck else 0
        yield
    finally:
        numba_config.CACHE_DIR, numba_config.BOUNDSCHECK = old_dir, old_bc


# ---- book depth --------------------------------------------------------------------------------------

_DEPTH_FIELDS = frozenset(n for n in abi.BOOK_DTYPE.names if n.startswith(("bid_", "ask_")))
_PLAIN = (int, float, complex, bool, str, bytes, type(None))
_ARRAY_MODULES = ("numpy", "math", "cmath")


def _followable(value: Any, pending: List[Any]) -> bool:
    """Whether a global or closure value cannot read the book's levels (a numba.njit function is
    queued for its own scan). Everything unknown is assumed to read them."""
    if isinstance(value, _PLAIN):
        return True
    if isinstance(value, tuple):
        return all(isinstance(v, _PLAIN) for v in value)
    if isinstance(value, numba.core.dispatcher.Dispatcher):
        pending.append(value.py_func)
        return True
    if isinstance(value, pytypes.ModuleType):
        root = value.__name__.split(".")[0]
        return root in _ARRAY_MODULES or root == "numba" or value.__name__ in ("fastmm", "fastmm.fx")
    module = getattr(value, "__module__", None) or ""
    if module == "fastmm.fx":
        return True
    if module.split(".")[0] in (*_ARRAY_MODULES, "builtins") and not isinstance(value, pytypes.FunctionType):
        return True  # numpy ufuncs, dtypes and builtins
    return False


def uses_book_depth(functions: List[Any]) -> bool:
    """Whether the hooks, or the numba.njit functions they reach, may read the book's level arrays:
    a level field's name among the names or string constants of their code, or a global or closure
    value that cannot be followed."""
    pending = list(functions)
    seen = set()
    while pending:
        fn = pending.pop()
        if id(fn) in seen:
            continue
        seen.add(id(fn))
        code_objects = [fn.__code__]
        while code_objects:
            code = code_objects.pop()
            if _DEPTH_FIELDS.intersection(code.co_names):
                return True
            for const in code.co_consts:
                if isinstance(const, str) and const in _DEPTH_FIELDS:
                    return True
                if isinstance(const, pytypes.CodeType):
                    code_objects.append(const)
            for name in code.co_names:
                if name in fn.__globals__ and not _followable(fn.__globals__[name], pending):
                    return True
        for cell in fn.__closure__ or ():
            try:
                value = cell.cell_contents
            except ValueError:  # an empty cell
                continue
            if not _followable(value, pending):
                return True
    return False


# ---- compile -------------------------------------------------------------------------------------------

def _make_entry(user: Any) -> Any:
    def entry(self_p, ctx_p, book_p):  # type: ignore[no-untyped-def]
        s = carray(self_p, 1)[0]
        ctx = carray(ctx_p, 1)[0]
        book = carray(book_p, 1)[0]
        try:
            user(s, ctx, book)
        except Exception:  # noqa: BLE001
            return _EXCEPTION
        return ctx.status
    return entry


class CompiledHot:
    """The compiled hooks of one class. Holds the cfunc objects, which own the machine code."""

    def __init__(self, spec: HotSpec, cache: bool, boundscheck: bool = True) -> None:
        # boundscheck=False exists to measure the cost of the checks (bench/python).
        self.spec = spec
        self.options = dict(JIT_OPTIONS, boundscheck=boundscheck)
        self.dtype, self.param_bytes = spec.record_dtype()
        self.cfuncs: Dict[str, Any] = {}
        self.seconds: Dict[str, float] = {}
        self.cache_hits: Dict[str, int] = {}
        self.cache = cache
        self.timer_names = list(spec.timers)
        record_t = numba.from_dtype(self.dtype)
        sig = types.int32(types.CPointer(record_t), types.CPointer(CTX_TYPE),
                          types.CPointer(BOOK_TYPE))
        with _compile_settings(cache, boundscheck):
            for name, hook in {**spec.hooks, **spec.timers}.items():
                self._compile(name, hook.fn, sig, cache)
        self.book_depth = uses_book_depth([h.fn for h in {**spec.hooks, **spec.timers}.values()])

    def _compile(self, name: str, fn: Any, sig: Any, cache: bool) -> None:
        where = f"{self.spec.qualname}.{name}"
        t0 = time.perf_counter()
        try:
            try:
                user = njit(cache=cache, **self.options)(fn)
            except RuntimeError as e:  # no source file to key a cache on (REPL, exec)
                if "cannot cache" not in str(e):
                    raise
                user = njit(**self.options)(fn)
            entry = cfunc(sig, error_model="python")(_make_entry(user))
        except (NumbaError, UnsupportedBytecodeError) as e:  # the latter is no NumbaError
            raise HotCompileError(f"fastmm: {where} does not compile in Numba nopython mode:\n"
                                  f"{e}") from None
        bad = disallowed_calls(entry.inspect_llvm())
        if bad:
            detail = "; ".join(f"{s} ({_explain(s)})" for s in bad)
            raise HotCompileError(f"fastmm: {where} is rejected by the IR check: it calls {detail}. "
                                  "Hot hooks may not allocate, print or use Python objects.")
        self.seconds[name] = time.perf_counter() - t0
        self.cache_hits[name] = sum(user.stats.cache_hits.values())
        self.cfuncs[name] = entry

    def program(self, instance: Any) -> Dict[str, Any]:
        """The dict fastmm._core._run_hot_strategy takes."""
        return {
            "hooks": {n: self.cfuncs[n].address for n in self.spec.hooks},
            "timers": [(self.cfuncs[n].address, self.spec.timers[n].period_ns)
                       for n in self.timer_names],
            "record": self.spec.initial_record(instance),
            "param_bytes": self.param_bytes,
            "book_depth": self.book_depth,
        }


def compiled(cls: type, spec: HotSpec, cache: bool) -> CompiledHot:
    """The class's compiled hooks, compiled on first use per cache setting."""
    store: Dict[bool, CompiledHot] = cls.__dict__.get("_fastmm_hot_compiled") or {}
    if cache not in store:
        store[cache] = CompiledHot(spec, cache)
        type.__setattr__(cls, "_fastmm_hot_compiled", store)
    return store[cache]


# ---- run -----------------------------------------------------------------------------------------------

_WHAT = {
    abi.EXCEPTION: "raised an exception",
    abi.FAILED: "called ctx.fail",
    abi.BAD_VALUE: "set a float price or quantity that is not finite, out of range or negative",
}


def run(config: Any, data: Any, instance: Any, name: str, spec: HotSpec, cache: bool,
        params: Mapping[str, str]) -> Tuple[Any, Optional[Dict[str, Any]], int]:
    """(result, error, hook calls); error is None or a dict describing the failure."""
    program = compiled(type(instance), spec, cache).program(instance)
    result, error, calls = _core._run_hot_strategy(config, data, name, dict(params), program)
    if error is None:
        return result, None, calls
    status, code, hook, timer, at_ns, events, kill_reason = error
    hook_name = hook or compiled(type(instance), spec, cache).timer_names[timer]
    what = _WHAT.get(status, f"returned status {status}")
    if status == abi.FAILED:
        what = f"called ctx.fail({code})"
    return result, {"hook": hook_name, "status": status, "fail_code": code, "now_ns": at_ns,
                    "events": events, "kill_reason": kill_reason, "what": what}, calls
