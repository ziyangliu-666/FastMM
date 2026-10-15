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

SCHEMA_VERSION = 10
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
        # Schema 10: the account each fill executed on, and how much of it is not an execution of
        # its own (an estimate executions named, or the part of an execution that named one).
        extra = " account, named_qty_raw," if self.schema_version >= 10 else ""
        return self.query(
            "SELECT session_id, seq, ts_ns, day," + extra + " symbol, side, liquidity, price_raw,"
            " qty_raw, booked_qty_raw, fee_raw, fee_asset, cl_ord_id, venue_order_id, exec_id,"
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
        books: bool = False,
    ) -> "pd.DataFrame":
        """Realised (with the funding in it, schema 4), fees and net by UTC day.

        `by` is "instrument" (a row per day, account and symbol) or "currency" (a row per day and
        settlement currency). Unrealised PnL is not summed across days: it is a mark, not a flow.

        From schema 10 the PnL is recomputed from the stored fills, as `fastmm-pnl pnl` does:
        average cost, by the account holding the position and symbol, carried across sessions;
        an execution counts once however many records hold it, and an estimate (a synthetic fill)
        only for what no execution named. `estimates` counts the estimates still open and
        `books_net` is the engine's own net for the day. `books=True` (or an older store) reads the
        engine's day roll-up of its position records instead.
        """
        if by not in ("instrument", "currency"):
            raise ValueError('by must be "instrument" or "currency"')
        if instrument and by == "currency":
            raise ValueError('instrument applies to by="instrument"')
        if self.schema_version >= 10 and not books:
            frame = self._ledger_pnl(since, until, instrument)
            if by == "instrument":
                return frame
            sums = ["realized", "funding", "fees", "net", "fills", "estimates", "books_net"]
            return (
                frame.groupby(["day", "settlement_ccy"], as_index=False)[sums]
                .sum()
                .sort_values(["day", "settlement_ccy"])
                .reset_index(drop=True)
            )
        clauses, params = [], []
        if since:
            clauses.append("day >= ?")
            params.append(since)
        if until:
            clauses.append("day <= ?")
            params.append(until)
        if instrument:
            clauses.append("symbol = ?")
            params.append(instrument)
        view = "pnl_by_day" if by == "instrument" else "pnl_by_currency"
        if by == "instrument":
            order = "day, account, symbol" if self.schema_version >= 10 else "day, symbol"
        else:
            order = "day, settlement_ccy"
        return self.query(f"SELECT * FROM {view}" + _where(clauses) + f" ORDER BY {order}", params)

    def _ledger_pnl(
        self, since: Optional[str], until: Optional[str], instrument: Optional[str]
    ) -> "pd.DataFrame":
        """The fills' PnL by day, account and symbol (src/store/sqlite_reader.cpp, pnl())."""
        pd = _pandas()
        scale = 1e8
        sym = " WHERE f.symbol = ?" if instrument else ""
        params = (instrument,) if instrument else ()
        rows = self._db.execute(
            "SELECT f.session_id, f.day, s.engine, COALESCE(NULLIF(i.account, ''), f.account),"
            " f.account, f.symbol, f.side, f.price_raw, f.qty_raw, f.booked_qty_raw,"
            " f.named_qty_raw, f.fee_raw, f.synthetic, f.exec_id, f.position_qty_raw,"
            " f.position_avg_px_raw, f.position_realized_raw - f.position_fees_raw,"
            " COALESCE(i.settlement_ccy, ''), COALESCE(i.inverse, 0),"
            " COALESCE(NULLIF(i.multiplier_raw, 0), 100000000), f.instrument_id"
            " FROM fills f JOIN sessions s ON s.session_id = f.session_id"
            " LEFT JOIN instruments i ON i.session_id = f.session_id"
            " AND i.instrument_id = f.instrument_id" + sym + " ORDER BY f.ts_ns, f.session_id, f.seq",
            params,
        )
        days: Dict[Any, Dict[str, Any]] = {}

        def bucket(key, ccy):
            b = days.get(key)
            if b is None:
                b = days[key] = dict(
                    realized=0.0, funding=0, fees=0, gross=0, fills=0, estimates=0,
                    books=None, ccy=ccy,
                )
            if ccy:
                b["ccy"] = ccy
            return b

        def wanted(day):
            return (not since or day >= since) and (not until or day <= until)

        books: Dict[Any, list] = {}
        seen = set()
        last: Dict[Any, int] = {}
        for (session, day, engine, holder, account, symbol, side, px, qty, booked, named, fee,
             synthetic, exec_id, pos_qty, pos_avg, engine_net, ccy, inverse, mult_raw,
             inst) in rows:
            prev_net = last.get((session, inst), 0)
            last[(session, inst)] = engine_net
            if synthetic:
                moved = qty - named
            else:
                key = (engine, account, symbol, side, exec_id)
                if exec_id and key in seen:
                    continue  # another session stored it too
                seen.add(key)
                moved = booked + named
            out = bucket((day, holder, symbol), ccy)
            counted = wanted(day)
            book = books.get((engine, holder, symbol))
            if book is None:
                books[(engine, holder, symbol)] = [pos_qty, float(pos_avg)]
                if counted:
                    out["realized"] += engine_net - prev_net + fee
                    out["fees"] += fee
            elif moved > 0:
                d = -moved if side == "Sell" else moved
                held, avg = book
                realized = 0.0
                if held == 0 or (held > 0) == (d > 0):
                    book[1] = (avg * abs(held) + px * moved) / (abs(held) + moved)
                    book[0] = held + d
                else:
                    closed = min(moved, abs(held))
                    sign = 1.0 if held > 0 else -1.0
                    mult = mult_raw / scale
                    if inverse:
                        realized = (
                            sign * closed * mult * (scale / avg - scale / px)
                            if avg > 0 and px > 0
                            else 0.0
                        )
                    else:
                        realized = sign * closed * (px - avg) / scale * mult
                    book[0] = held + d
                    if book[0] == 0:
                        book[1] = 0.0
                    elif (book[0] > 0) != (held > 0):
                        book[1] = float(px)
                if counted:
                    out["realized"] += realized
                    if not synthetic:
                        out["fees"] += fee
            if counted:
                if moved > 0:
                    out["gross"] += moved
                if synthetic:
                    out["estimates"] += 1 if moved > 0 else 0
                else:
                    out["fills"] += 1
        paid = set()
        for session, day, engine, holder, symbol, fid, amount, ccy in self._db.execute(
            "SELECT g.session_id, g.day, s.engine, COALESCE(NULLIF(i.account, ''), g.account),"
            " g.symbol, g.funding_id, g.amount_raw, COALESCE(i.settlement_ccy, '')"
            " FROM funding g JOIN sessions s ON s.session_id = g.session_id"
            " LEFT JOIN instruments i ON i.session_id = g.session_id"
            " AND i.instrument_id = g.instrument_id"
            + sym.replace("f.", "g.")
            + " ORDER BY g.ts_ns, g.session_id, g.seq",
            params,
        ):
            if fid and (engine, holder, symbol, fid) in paid:
                continue
            paid.add((engine, holder, symbol, fid))
            if not wanted(day):
                continue
            out = bucket((day, holder, symbol), ccy)
            out["funding"] += amount
            out["realized"] += amount
        for day, holder, symbol, ccy, net in self._db.execute(
            "SELECT day, account, symbol, settlement_ccy, SUM(realized_raw) - SUM(fees_raw)"
            " FROM pnl_daily" + (" WHERE symbol = ?" if instrument else "")
            + " GROUP BY day, account, symbol, settlement_ccy",
            params,
        ):
            if not wanted(day):
                continue
            out = bucket((day, holder, symbol), ccy)
            out["books"] = (out["books"] or 0) + net
        records = []
        for (day, holder, symbol), b in sorted(days.items()):
            if not (b["fills"] or b["estimates"] or b["funding"] or b["books"] is not None):
                continue
            realized = round(b["realized"])
            records.append(
                dict(
                    day=day, account=holder, symbol=symbol, settlement_ccy=b["ccy"],
                    realized=realized / scale, funding=b["funding"] / scale,
                    fees=b["fees"] / scale, net=(realized - b["fees"]) / scale,
                    gross_traded=b["gross"] / scale, fills=b["fills"],
                    estimates=b["estimates"],
                    books_net=None if b["books"] is None else b["books"] / scale,
                )
            )
        columns = ["day", "account", "symbol", "settlement_ccy", "realized", "funding", "fees",
                   "net", "gross_traded", "fills", "estimates", "books_net"]
        return pd.DataFrame.from_records(records, columns=columns)

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
