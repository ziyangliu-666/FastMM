"""Publishes (ADR-0013, section 1): names, types, ranges and validate() are checked in the calling
thread on a copy of the parameters; the channel receives schema indices and raw values."""

from __future__ import annotations

import copy
import struct
import threading
from typing import TYPE_CHECKING, Any, Dict, List, Mapping, Optional

from .._hot.decl import fixed_raw
from . import abi
from .context import instrument_index

if TYPE_CHECKING:  # pragma: no cover
    from .._hot.decl import HotSpec

_DOUBLE = struct.Struct("<d")
_INT64 = struct.Struct("<q")


def _raw(kind: type, value: Any) -> int:
    if kind is bool:
        return 1 if value else 0
    if kind is int:
        return int(value)
    return _INT64.unpack(_DOUBLE.pack(float(value)))[0]


class Publisher:
    """Validates and sends publishes for one session. Thread-safe; the values it keeps are the ones
    it sent, which the engine holds once it has applied them."""

    def __init__(self, cls: type, spec: "HotSpec", instance: Any, channel: Any) -> None:
        self._cls = cls
        self._params = spec.params
        self._states = spec.states
        self._index = {name: i for i, name in enumerate(spec.params)}
        self._instance = instance
        self._channel = channel
        self._symbols: List[str] = list(channel.symbols)
        base = {name: getattr(instance, name) for name in spec.params}
        self._values: List[Dict[str, Any]] = [dict(base) for _ in range(channel.instruments)]
        self._lock = threading.Lock()

    def values(self, inst: Any = 0) -> Dict[str, Any]:
        """The parameter values last sent for one instrument."""
        k = instrument_index(inst, self._symbols)
        with self._lock:
            return dict(self._values[k])

    def publish(self, inst: Any, values: Mapping[str, Any]) -> bool:
        if len(values) > abi.MAX_FIELDS:
            raise ValueError(f"at most {abi.MAX_FIELDS} parameters per update, got {len(values)}")
        k = -1 if inst is None else instrument_index(inst, self._symbols)
        typed: Dict[str, Any] = {}
        for name, value in values.items():
            p = self._params.get(name)
            if p is None:
                if name in self._states:
                    raise ValueError(f"'{name}' is a State field; publish takes parameters")
                raise ValueError(f"unknown parameter '{name}'")
            try:
                err, v = p.parse(value)
            except TypeError as e:
                raise ValueError(f"parameter '{name}': {e}") from None
            if err is not None:
                raise ValueError(f"parameter '{name}': {err}")
            if p.type is float:
                fixed_raw(v, name)  # ValueError when it does not fit in 1e-8 fixed point
            typed[name] = v
        with self._lock:
            targets = range(len(self._values)) if k < 0 else (k,)
            for t in targets:
                err = self._validate({**self._values[t], **typed})
                if err:
                    where = f"instrument {self._symbols[t] or t}: " if len(self._values) > 1 else ""
                    raise ValueError(f"{where}{err}")
            fields = [self._index[name] for name in typed]
            raws = [_raw(self._params[name].type, v) for name, v in typed.items()]
            ok = bool(self._channel.publish(k, fields, raws))
            if ok:
                for t in targets:
                    self._values[t].update(typed)
            return ok

    def _validate(self, merged: Mapping[str, Any]) -> Optional[str]:
        shadow = copy.copy(self._instance)
        shadow.__dict__.update(merged)
        err = self._cls.validate(shadow)
        return str(err) if err else None
