"""Read a FastMM store: the fills, orders, positions and PnL a live session recorded.

The store is what `[storage]` wrote while the session ran; the journal next to it is the
byte-exact event stream a replay consumes. See docs/reference/storage.md for the schema.

    import fastmm

    with fastmm.open_store("runs/mm1.db") as store:
        store.pnl(since="2024-03-04")          # realised, fees and net by day and instrument
        store.fills(session=1709510400000000)  # every execution of one session
"""

from __future__ import annotations

import sqlite3
from typing import TYPE_CHECKING, Any, Dict, Iterable, Optional, Sequence, Union

if TYPE_CHECKING:  # pragma: no cover - typing only
    import os

    import pandas as pd

FIXED_SCALE = 1e-8
"""Scale of the raw int64 fixed-point columns (`price_raw`, `qty_raw`, `realized_raw`, ...)."""

SCHEMA_VERSION = 7
"""Schema version this module reads; a newer store is refused."""

_NS_COLUMNS = frozenset(
    {"ts_ns", "started_ns", "stopped_ns", "created_ns", "updated_ns", "last_ns"}
)


def _pandas():
    try:
        import pandas as pd
    except ImportError as exc:  # pragma: no cover - exercised only without pandas
        raise ImportError(
            "fastmm.open_store() needs pandas; install it with `pip install 'pandas>=2.0'`"
        ) from exc
    return pd


def _frame(pd, cursor: sqlite3.Cursor) -> "pd.DataFrame":
    """Builds a frame from a cursor, decoding raw fixed point and nanosecond columns."""
    names = [d[0] for d in cursor.description]
    df = pd.DataFrame(cursor.fetchall(), columns=names)
    rename: Dict[str, str] = {}
    for name in names:
        if name.endswith("_raw"):
            df[name] = df[name].astype("float64") * FIXED_SCALE
            rename[name] = name[: -len("_raw")]
        elif name in _NS_COLUMNS:
            df[name] = pd.to_datetime(df[name], unit="ns", utc=True)
            rename[name] = name[: -len("_ns")]
    return df.rename(columns=rename)


def _where(clauses: Sequence[str]) -> str:
    return " WHERE " + " AND ".join(clauses) if clauses else ""


class Store:
    """A read-only connection to a FastMM SQLite store.

    Every method returns a pandas DataFrame with raw fixed-point columns decoded to floats and
    nanosecond timestamps decoded to UTC datetimes, so `fee_raw` reads back as `fee`.
    """

    def __init__(self, path: Union[str, "os.PathLike[str]"]):
        self.path = str(path)
        # uri=True with mode=ro: opening a store that is not there is an error, not an empty file.
        self._db = sqlite3.connect(f"file:{self.path}?mode=ro", uri=True)
        self._db.row_factory = None
        try:
            (version,) = self._db.execute("SELECT version FROM schema_version").fetchone()
        except sqlite3.DatabaseError as exc:
            self.close()
            raise ValueError(f"{self.path} is not a FastMM store") from exc
        if version > SCHEMA_VERSION:
            self.close()
            raise ValueError(
                f"{self.path} has schema version {version}; this fastmm reads {SCHEMA_VERSION}"
            )
        self.schema_version = int(version)

    def __enter__(self) -> "Store":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    def close(self) -> None:
        """Closes the connection. The store is read-only, so nothing is flushed."""
        if getattr(self, "_db", None) is not None:
            self._db.close()
            self._db = None  # type: ignore[assignment]

    def query(self, sql: str, params: Iterable[Any] = ()) -> "pd.DataFrame":
        """Runs `sql` and returns the rows, decoding `_raw` and `_ns` columns as above."""
        pd = _pandas()
        return _frame(pd, self._db.execute(sql, tuple(params)))

    def sessions(
        self, engine: Optional[str] = None, since: Optional[str] = None, until: Optional[str] = None
    ) -> "pd.DataFrame":
        """One row per session: when it ran, what it made and how it ended."""
        clauses, params = [], []
        if engine:
            clauses.append("engine = ?")
            params.append(engine)
        if since:
            clauses.append("started_day >= ?")
            params.append(since)
        if until:
            clauses.append("started_day <= ?")
            params.append(until)
        return self.query(
            "SELECT session_id, engine, strategy, started_ns, stopped_ns, dry_run, clean_shutdown,"
            " exit_code, kill_reason, kill_latched, fills, realized_raw, unrealized_raw, fees_raw,"
            " records_dropped, journal_complete FROM sessions"
            + _where(clauses)
            + " ORDER BY started_ns",
            params,
        )

    def fills(
        self,
        session: Optional[int] = None,
        instrument: Optional[str] = None,
        since: Optional[str] = None,
        until: Optional[str] = None,
    ) -> "pd.DataFrame":
        """One row per execution, in time order."""
        clauses, params = [], []
        if session:
            clauses.append("session_id = ?")
            params.append(session)
        if instrument:
            clauses.append("symbol = ?")
            params.append(instrument)
        if since:
            clauses.append("day >= ?")
            params.append(since)
        if until:
            clauses.append("day <= ?")
            params.append(until)
        return self.query(
            "SELECT session_id, seq, ts_ns, day, symbol, side, liquidity, price_raw, qty_raw,"
            " booked_qty_raw, fee_raw, fee_asset, cl_ord_id, venue_order_id, exec_id,"
            " position_qty_raw, synthetic, late FROM fills" + _where(clauses) + " ORDER BY ts_ns",
            params,
        )

    def orders(
        self,
        session: Optional[int] = None,
        instrument: Optional[str] = None,
        open_only: bool = False,
    ) -> "pd.DataFrame":
        """One row per order, in the last state the session recorded for it."""
        clauses, params = [], []
        if session:
            clauses.append("session_id = ?")
            params.append(session)
        if instrument:
            clauses.append("symbol = ?")
            params.append(instrument)
        if open_only:
            clauses.append("terminal = 0")
        return self.query(
            "SELECT session_id, cl_ord_id, symbol, side, type, tif, price_raw, qty_raw,"
            " cum_qty_raw, state, reject_reason, terminal, updates, created_ns, updated_ns"
            " FROM orders" + _where(clauses) + " ORDER BY updated_ns",
            params,
        )

    def positions(self, session: Optional[int] = None) -> "pd.DataFrame":
        """The last position snapshot of each session and instrument."""
        clauses, params = [], []
        if session:
            clauses.append("p.session_id = ?")
            params.append(session)
        return self.query(
            "SELECT p.session_id, p.ts_ns, p.symbol, p.qty_raw, p.avg_px_raw, p.realized_raw,"
            " p.unrealized_raw, p.fees_raw, p.fills FROM positions p JOIN"
            " (SELECT session_id, instrument_id, MAX(seq) AS seq FROM positions"
            "  GROUP BY session_id, instrument_id) l"
            " ON p.session_id = l.session_id AND p.seq = l.seq"
            + _where(clauses)
            + " ORDER BY p.session_id, p.symbol",
            params,
        )

    def pnl(
        self,
        since: Optional[str] = None,
        until: Optional[str] = None,
        instrument: Optional[str] = None,
        by: str = "instrument",
    ) -> "pd.DataFrame":
        """Realised (with the funding in it, schema 4), fees and net by UTC day.

        `by` is "instrument" (a row per day and symbol) or "currency" (a row per day and
        settlement currency). Unrealised PnL is not summed across days: it is a mark, not a flow.
        """
        if by not in ("instrument", "currency"):
            raise ValueError('by must be "instrument" or "currency"')
        clauses, params = [], []
        if since:
            clauses.append("day >= ?")
            params.append(since)
        if until:
            clauses.append("day <= ?")
            params.append(until)
        if instrument:
            if by == "currency":
                raise ValueError('instrument applies to by="instrument"')
            clauses.append("symbol = ?")
            params.append(instrument)
        view = "pnl_by_day" if by == "instrument" else "pnl_by_currency"
        order = "day, symbol" if by == "instrument" else "day, settlement_ccy"
        return self.query(f"SELECT * FROM {view}" + _where(clauses) + f" ORDER BY {order}", params)

    def funding(
        self,
        session: Optional[int] = None,
        instrument: Optional[str] = None,
        since: Optional[str] = None,
        until: Optional[str] = None,
    ) -> "pd.DataFrame":
        """Every perpetual funding payment booked: `amount` in `asset`, negative paid.

        Funding is realized PnL; `pnl()` and `positions()` carry it inside `realized` and say how
        much of it in `funding`. A store from before schema 4 has none.
        """
        if self.schema_version < 4:
            return self.query("SELECT 1 AS none WHERE 0")
        clauses, params = [], []
        if session:
            clauses.append("session_id = ?")
            params.append(session)
        if instrument:
            clauses.append("symbol = ?")
            params.append(instrument)
        if since:
            clauses.append("day >= ?")
            params.append(since)
        if until:
            clauses.append("day <= ?")
            params.append(until)
        return self.query(
            "SELECT session_id, ts_ns, exch_ns, symbol, amount_raw, asset, funding_id,"
            " position_qty_raw, position_funding_raw, replayed FROM funding"
            + _where(clauses)
            + " ORDER BY ts_ns",
            params,
        )

    def duplicates(
        self, engine: Optional[str] = None, instrument: Optional[str] = None
    ) -> "pd.DataFrame":
        """One row per venue execution or funding payment that more than one session stored.

        A restart booked it a second time: the positions, fees and PnL of the sessions holding
        the copies count it twice. An execution is the venue's id of it on one venue, symbol and
        side. Empty in a sound store. The columns are those of `fastmm-pnl duplicates`: kind
        (`fill` or `funding`), engine, venue, symbol, id, copies, side, qty (the amount of a
        funding payment), ts (of the first copy) and sessions (oldest first, space separated).
        """
        venue = (
            "COALESCE(v.name, '#' || r.venue_id)"
            if self.schema_version >= 3
            else "'#' || r.venue_id"
        )
        names = (
            " LEFT JOIN session_venues v ON v.session_id = r.session_id"
            " AND v.venue_id = r.venue_id"
            if self.schema_version >= 3
            else ""
        )

        def part(kind: str, table: str, ident: str, side: str, qty: str) -> str:
            key = "symbol, side" if kind == "fill" else "symbol"
            on_side = " AND r.side = c.side" if kind == "fill" else ""
            select = (
                f"SELECT '{kind}' AS kind, s.engine AS engine, {venue} AS venue,"
                f" r.symbol AS symbol, r.{ident} AS id, r.session_id AS session_id,"
                f" s.started_ns AS started_ns, r.ts_ns AS ts_ns, {side} AS side,"
                f" r.{qty} AS qty_raw FROM "
            )
            sessions = f" JOIN sessions s ON s.session_id = r.session_id{names}"
            if self.schema_version < 7:  # no id index: every row is grouped
                return select + f"{table} r" + sessions + f" WHERE r.{ident} <> ''"
            # The repeated ids first, from the id index of schema 7, then their rows by id.
            return (
                select
                + f"(SELECT {ident} AS id, {key} FROM {table} WHERE {ident} <> ''"
                f" GROUP BY {ident}, {key} HAVING COUNT(*) > 1) c"
                f" CROSS JOIN {table} r ON r.{ident} = c.id AND r.symbol = c.symbol{on_side}"
                f" AND r.{ident} <> ''" + sessions
            )

        rows = part("fill", "fills", "exec_id", "r.side", "qty_raw")
        if self.schema_version >= 4:
            rows += " UNION ALL " + part("funding", "funding", "funding_id", "''", "amount_raw")
        clauses, params = [], []
        if engine:
            clauses.append("engine = ?")
            params.append(engine)
        if instrument:
            clauses.append("symbol = ?")
            params.append(instrument)
        return self.query(
            "SELECT kind, engine, venue, symbol, id, COUNT(*) AS copies, MIN(side) AS side,"
            " MIN(qty_raw) AS qty_raw, MIN(ts_ns) AS ts_ns,"
            " GROUP_CONCAT(session_id, ' ') AS sessions"
            f" FROM (SELECT * FROM ({rows}) ORDER BY started_ns)"
            + _where(clauses)
            + " GROUP BY kind, engine, venue, symbol, side, id HAVING COUNT(*) > 1"
            " ORDER BY MIN(ts_ns)",
            params,
        )

    def kill_events(self, session: Optional[int] = None) -> "pd.DataFrame":
        """Every kill switch trip, global or per venue, with the reason and the PnL at the time."""
        clauses, params = [], []
        if session:
            clauses.append("session_id = ?")
            params.append(session)
        return self.query(
            "SELECT session_id, ts_ns, scope, venue_id, reason, kill_flags, realized_raw,"
            " unrealized_raw, fees_raw FROM kill_events" + _where(clauses) + " ORDER BY ts_ns",
            params,
        )

    def journals(self, session: Optional[int] = None) -> "pd.DataFrame":
        """The journal parts each session wrote, in order."""
        clauses, params = [], []
        if session:
            clauses.append("session_id = ?")
            params.append(session)
        return self.query(
            "SELECT session_id, part, path FROM session_journals"
            + _where(clauses)
            + " ORDER BY session_id, part",
            params,
        )


def open_store(path: Union[str, "os.PathLike[str]"]) -> Store:
    """Opens a FastMM store read-only.

    Args:
        path: the store file, `[storage] path` (default `runs/<engine name>.db`).

    Returns:
        A `Store` whose methods return pandas DataFrames. Use it as a context manager to close
        the connection.

    Raises:
        ValueError: the file is not a FastMM store, or was written by a newer FastMM.
        sqlite3.OperationalError: the file does not exist or cannot be read.
    """
    return Store(path)
