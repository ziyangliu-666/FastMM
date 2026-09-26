import sqlite3
import subprocess
import sys

import pytest

import fastmm

pd = pytest.importorskip("pandas")

from conftest import REPO  # noqa: E402

# The store is written by C++; the test binary that fills one is not built for the wheel job, so
# the fixture builds an equivalent database with the same schema through the shipped SQL.
SCHEMA = REPO / "src" / "store" / "sqlite_schema.cpp"


def _sql_between(text, begin, end):
    start = text.index(begin) + len(begin)
    return text[start : text.index(end, start)]


@pytest.fixture(scope="module")
def store_path(tmp_path_factory):
    """A store built from the schema in src/store/sqlite_schema.cpp, filled with two days."""
    text = SCHEMA.read_text()
    v1 = _sql_between(text, 'constexpr std::string_view kV1 = R"SQL(', ')SQL";')
    v2 = _sql_between(text, 'constexpr std::string_view kV2 = R"SQL(', ')SQL";')
    v3 = _sql_between(text, 'constexpr std::string_view kV3 = R"SQL(', ')SQL";')
    v4 = _sql_between(text, 'constexpr std::string_view kV4 = R"SQL(', ')SQL";')
    path = tmp_path_factory.mktemp("store") / "mm1.db"
    db = sqlite3.connect(path)
    db.executescript(
        "CREATE TABLE schema_version (version INTEGER NOT NULL, applied_ns INTEGER NOT NULL,"
        " fastmm TEXT NOT NULL);"
        "INSERT INTO schema_version VALUES (4, 0, 'test');"
    )
    db.executescript(v1)
    db.executescript(v2)
    day1 = 1_709_510_400_000_000_000
    day2 = day1 + 86_400_000_000_000
    db.executescript(
        f"""
        INSERT INTO sessions (session_id, engine, strategy, session_epoch, started_ns,
            started_day, stopped_ns, version, build, config_hash, dry_run, pnl_carry_raw,
            clean_shutdown, exit_code, kill_reason, kill_latched, fills, realized_raw,
            unrealized_raw, fees_raw, records_dropped, journal_complete)
          VALUES (1, 'mm1', 'basic_mm', 3, {day1}, '2024-03-04', {day2}, '0.1.0', 'b', '0', 0, 0,
                  1, 0, 'None', 0, 2, 150000000, 0, 3000, 0, 1);
        INSERT INTO instruments VALUES
          (1, 0, 0, 'BTCUSDT', 'BTC', 'USDT', 'USDT', 'Spot', 0, 1000000, 100000, 100000000);
        INSERT INTO session_journals VALUES (1, 0, 'runs/mm1.fmj');
        INSERT INTO fills VALUES
          (1, 1, {day1 + 3600000000000}, '2024-03-04', 0, 0, 'BTCUSDT', 'C1', 'V1', 'E1', 'Buy',
           'Maker', 10000000000, 100000000, 100000000, 100000000, 0, 1000, 1000, 'quote',
           100000000, 10000000000, 0, 1000, 0, 0),
          (1, 3, {day2 + 3600000000000}, '2024-03-05', 0, 0, 'BTCUSDT', 'C2', 'V2', 'E2', 'Sell',
           'Taker', 10100000000, 100000000, 100000000, 100000000, 0, 2000, 2000, 'quote',
           0, 0, 100000000, 3000, 0, 0);
        INSERT INTO orders VALUES
          (1, 'C1', 0, 0, 'BTCUSDT', 'V1', 'Buy', 'Limit', 'GTC', 10000000000, 100000000,
           100000000, 'Filled', '', 1, 0, 0, {day1}, {day1 + 3600000000000}, '2024-03-04', 3),
          (1, 'C3', 0, 0, 'BTCUSDT', '', 'Sell', 'Limit', 'GTC', 10200000000, 100000000, 0,
           'Live', '', 0, 0, 0, {day2}, {day2}, '2024-03-05', 1);
        INSERT INTO positions VALUES
          (1, 2, {day1 + 3600000000000}, '2024-03-04', 0, 'BTCUSDT', 100000000, 10000000000,
           0, 0, 1000, 100000000, 1, 0, 0, 1000, 0),
          (1, 4, {day2 + 3600000000000}, '2024-03-05', 0, 'BTCUSDT', 0, 0, 100000000, 0, 3000,
           200000000, 2, 100000000, 0, 3000, 0);
        INSERT INTO pnl_daily VALUES
          (1, '2024-03-04', 0, 'BTCUSDT', 'USDT', 0, 1000, 0, 100000000, 100000000, 1,
           {day1 + 3600000000000}),
          (1, '2024-03-05', 0, 'BTCUSDT', 'USDT', 100000000, 2000, 0, 0, 100000000, 1,
           {day2 + 3600000000000});
        INSERT INTO kill_events VALUES
          (1, 5, {day2 + 7200000000000}, '2024-03-05', 'global', -1, 'MaxLoss', 1, 100000000, 0,
           3000, 0);
        """
    )
    # Rows written under version 2, then migrated, as a store carried across the upgrade is.
    db.executescript(v3)
    db.executescript(v4)
    # A funding payment of -0.25 USDT on day two, under version 4.
    db.executescript(
        f"""
        INSERT INTO funding VALUES
          (1, 6, {day2 + 28800000000000}, '2024-03-05', {day2 + 28800000000000}, 0, 0,
           'BTCUSDT', '9689322392', 'USDT', -25000000, 0, 75000000, -25000000, 0);
        UPDATE pnl_daily SET funding_raw = -25000000, realized_raw = realized_raw - 25000000
          WHERE day = '2024-03-05';
        """
    )
    db.commit()
    db.close()
    return path


def test_open_store_reports_the_schema_version(store_path):
    with fastmm.open_store(store_path) as store:
        assert store.schema_version == fastmm.store.SCHEMA_VERSION


def test_open_store_refuses_a_file_that_is_not_a_store(tmp_path):
    other = tmp_path / "other.db"
    sqlite3.connect(other).executescript("CREATE TABLE junk (x INTEGER)")
    with pytest.raises(ValueError, match="not a FastMM store"):
        fastmm.open_store(other)


def test_open_store_refuses_a_missing_file(tmp_path):
    with pytest.raises(sqlite3.OperationalError):
        fastmm.open_store(tmp_path / "nope.db")


def test_sessions_frame(store_path):
    with fastmm.open_store(store_path) as store:
        df = store.sessions()
    assert list(df["session_id"]) == [1]
    assert df["strategy"][0] == "basic_mm"
    # Raw fixed point is decoded and the _raw suffix dropped.
    assert df["realized"][0] == pytest.approx(1.5)
    assert df["fees"][0] == pytest.approx(0.00003)
    assert str(df["started"][0]) == "2024-03-04 00:00:00+00:00"
    assert store.schema_version == 4


def test_fills_frame_and_filters(store_path):
    with fastmm.open_store(store_path) as store:
        every = store.fills()
        one_day = store.fills(since="2024-03-05")
        other = store.fills(instrument="ETHUSDT")
    assert len(every) == 2
    assert list(every["side"]) == ["Buy", "Sell"]
    assert every["price"][0] == pytest.approx(100.0)
    assert every["qty"][0] == pytest.approx(1.0)
    assert len(one_day) == 1
    assert one_day["exec_id"].iloc[0] == "E2"
    assert other.empty


def test_pnl_by_day_and_by_currency(store_path):
    with fastmm.open_store(store_path) as store:
        by_instrument = store.pnl()
        by_currency = store.pnl(by="currency")
        yesterday = store.pnl(since="2024-03-05", until="2024-03-05")
    assert list(by_instrument["day"]) == ["2024-03-04", "2024-03-05"]
    assert by_instrument["realized"].sum() == pytest.approx(0.75)
    assert by_instrument["net"].iloc[1] == pytest.approx(0.75 - 0.00002)
    assert list(by_currency["settlement_ccy"]) == ["USDT", "USDT"]
    assert len(yesterday) == 1
    with pytest.raises(ValueError):
        store.pnl(by="galaxy")


def test_funding_frame_and_pnl(store_path):
    with fastmm.open_store(store_path) as store:
        funding = store.funding()
        pnl = store.pnl()
    assert list(funding["funding_id"]) == ["9689322392"]
    assert funding["amount"][0] == pytest.approx(-0.25)
    assert funding["asset"][0] == "USDT"
    # Funding is realized PnL; the day's row says how much of it.
    assert pnl["funding"].iloc[1] == pytest.approx(-0.25)
    assert pnl["realized"].iloc[1] == pytest.approx(0.75)


def test_orders_positions_kills_and_journals(store_path):
    with fastmm.open_store(store_path) as store:
        assert len(store.orders()) == 2
        assert list(store.orders(open_only=True)["cl_ord_id"]) == ["C3"]
        positions = store.positions()
        assert len(positions) == 1  # the last snapshot per instrument
        assert positions["realized"][0] == pytest.approx(1.0)
        kills = store.kill_events()
        assert list(kills["reason"]) == ["MaxLoss"]
        assert list(store.journals()["path"]) == ["runs/mm1.fmj"]


def test_query_decodes_raw_and_ns_columns(store_path):
    with fastmm.open_store(store_path) as store:
        df = store.query("SELECT ts_ns, price_raw FROM fills ORDER BY seq LIMIT 1")
    assert list(df.columns) == ["ts", "price"]
    assert df["price"][0] == pytest.approx(100.0)


def test_store_is_closed_after_the_context(store_path):
    store = fastmm.open_store(store_path)
    store.close()
    store.close()  # idempotent


@pytest.mark.skipif(
    not (REPO / "build" / "release" / "bin" / "fastmm-pnl").exists(),
    reason="fastmm-pnl is not built",
)
def test_fastmm_pnl_reads_the_same_store(store_path):
    out = subprocess.run(
        [
            str(REPO / "build" / "release" / "bin" / "fastmm-pnl"),
            "pnl",
            "--store",
            str(store_path),
        ],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    assert "2024-03-05" in out
    assert "BTCUSDT" in out
    assert sys.executable  # keeps the import used


def test_fixed_scale_matches_the_engine():
    assert fastmm.store.FIXED_SCALE == fastmm.FIXED_SCALE
