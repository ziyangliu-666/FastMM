"""fastmm.replay: replays a Python hot strategy from a journal's ParamUpdate messages without running
its slow methods (ADR-0013, section 2)."""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any, Dict, List, Mapping, Optional

from .. import _core

if TYPE_CHECKING:  # pragma: no cover
    from .._core import BacktestConfig
    from .._hot.decl import HotSpec

_PARAM = "python.param."


@dataclass(frozen=True)
class ReplayResult:
    """Outcome of fastmm.replay(). `ok`: the replayed engine sent the recorded messages. `what_if`:
    the replay differs from the recording (`what_if_reasons`), so a mismatch is a result, not an
    error."""

    ok: bool
    outbound_sha256: str
    recorded_sha256: str
    outbound_messages: int
    recorded_messages: int
    events: int
    first_mismatch: int
    expected_message: str
    actual_message: str
    what_if: bool
    what_if_reasons: List[str] = field(default_factory=list)


def journal_metadata(cls: type, spec: "HotSpec", params: Mapping[str, str],
                     max_param_age_ms: int) -> str:
    """The journal metadata of a backtest of `cls`: class, hot source hash, versions, the effective
    max_param_age_ms and the initial parameters."""
    import numba

    from .._hot import compiler

    lines = [
        f"python.class={cls.__module__}:{cls.__qualname__}",
        f"python.hot_source={compiler.source_hash(spec)}",
        f"python.fastmm={_core.__version__}",
        f"python.numba={numba.__version__}",
        f"python.max_param_age_ms={max_param_age_ms}",
    ]
    lines += [f"{_PARAM}{name}={value}" for name, value in params.items()]
    return "\n".join(lines) + "\n"


def parse_metadata(text: str) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for line in text.splitlines():
        key, sep, value = line.partition("=")
        if sep:
            out[key] = value
    return out


def replay(journal: Any, strategy: type, *, verify: bool = True,
           config: "Optional[BacktestConfig]" = None, params: Optional[Mapping[str, Any]] = None,
           param_updates: bool = True, hot_cache: bool = True) -> ReplayResult:
    """Replay a journal recorded by a backtest of a Python hot strategy.

    The engine consumes the journal's inputs, ParamUpdate messages included, with `strategy`'s hot
    hooks; slow methods do not run. `verify` compares every outbound message with the recording.
    The parameters start at the values the journal records (`params` overrides them) and
    max_param_age_ms is the recorded one. `config` defaults to the configuration the journal
    embeds. A different class, hot-hook source, fastmm or numba version, `params` or
    `param_updates=False` makes it a what-if replay: ReplayResult.what_if is True and the reasons
    are listed."""
    from .._hot import compiler
    from ..strategy import Strategy

    if not (isinstance(strategy, type) and issubclass(strategy, Strategy)):
        raise TypeError("fastmm.replay takes a fastmm.Strategy subclass")
    spec = strategy._fastmm_hot
    if spec is None:
        raise TypeError(f"fastmm.replay: {strategy.__qualname__} has no @fastmm.hot hooks")
    path = os.fspath(journal)
    meta = parse_metadata(_core.inspect_journal(path).get("metadata", ""))
    reasons: List[str] = []

    import numba

    current = {
        "python.class": f"{strategy.__module__}:{strategy.__qualname__}",
        "python.hot_source": compiler.source_hash(spec),
        "python.fastmm": _core.__version__,
        "python.numba": numba.__version__,
    }
    if "python.class" not in meta:
        reasons.append("the journal records no Python strategy")
    else:
        labels = {"python.class": "class", "python.hot_source": "hot-hook source hash",
                  "python.fastmm": "fastmm version", "python.numba": "numba version"}
        for key, label in labels.items():
            if meta.get(key) != current[key]:
                reasons.append(f"{label}: recorded {meta.get(key)}, replaying {current[key]}")

    if config is None:
        try:
            config = _core._journal_config(path)
        except RuntimeError as e:
            raise RuntimeError(f"{e}; pass config= to fastmm.replay") from None
        if "python.max_param_age_ms" in meta:
            config.max_param_age_ms = int(meta["python.max_param_age_ms"])
    else:
        config = config.copy()
        recorded_age = meta.get("python.max_param_age_ms")
        if recorded_age is not None and int(recorded_age) != config.max_param_age_ms:
            reasons.append(f"max_param_age_ms: recorded {recorded_age}, replaying "
                           f"{config.max_param_age_ms}")

    declared = strategy.params()
    recorded = {k[len(_PARAM):]: v for k, v in meta.items() if k.startswith(_PARAM)}
    values: Dict[str, Any] = {}
    for name, value in recorded.items():
        if name in declared:
            values[name] = value
        else:
            reasons.append(f"recorded parameter '{name}' is not a parameter of "
                           f"{strategy.__qualname__}")
    for name in declared:
        if meta and name not in recorded and "python.class" in meta:
            reasons.append(f"parameter '{name}' is not in the recording")
    instance = strategy()
    instance.configure(values)
    if params:
        before = instance.param_values()
        instance.configure({**values, **params})
        after = instance.param_values()
        changed = sorted(k for k in after if after[k] != before[k])
        if changed:
            reasons.append(f"params= changes {', '.join(changed)}")
    if not param_updates:
        reasons.append("ParamUpdate messages skipped (param_updates=False)")

    program = compiler.compiled(strategy, spec, hot_cache).program(instance)
    d = _core._replay_hot_strategy(path, config, strategy.strategy_name(), program, verify,
                                   param_updates)
    return ReplayResult(
        ok=bool(d["ok"]), outbound_sha256=d["outbound_sha256"],
        recorded_sha256=d["recorded_sha256"], outbound_messages=int(d["outbound_messages"]),
        recorded_messages=int(d["recorded_messages"]), events=int(d["events"]),
        first_mismatch=int(d["first_mismatch"]), expected_message=d["expected_message"],
        actual_message=d["actual_message"], what_if=bool(reasons), what_if_reasons=reasons)
