#!/usr/bin/env python3
"""Generates the Nasdaq wire fixtures used by tests/codecs/*_test.cpp.

Independent of the C++ layouts on purpose: every message is packed here with struct.pack from
the offset / length tables of the official specifications, so a mistake in a C++ struct shows up
as a byte mismatch instead of being reproduced on both sides.

  TotalView-ITCH 5.0   NQTVITCHspecification.pdf (revision log 2023-04-28)
  OUCH 4.2             OUCH4.2.pdf (updated October 2025)
  OUCH 5.0             Ouch5.0.pdf (updated October 2025)
  SoupBinTCP           soupbintcp.pdf (3.00), SoupBinTCP 4.0.pdf, SoupBinTCP41.pdf
  MoldUDP64            moldudp64.pdf (V 1.00)

Output format (one file per protocol): '# comment' lines and '<name> <hex>' lines.
Run from anywhere: python3 tests/fixtures/nasdaq/generate_fixtures.py
"""
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))

TS = 34_200_000_000_123  # 09:30:00.000000123 in nanoseconds since midnight
LOCATE = 7
TRACKING = 3


def alpha(s, n):
    b = s.encode("ascii")
    assert len(b) <= n
    return b + b" " * (n - len(b))


def left_padded(s, n):
    b = s.encode("ascii")
    assert len(b) <= n
    return b" " * (n - len(b)) + b


def ts48(v):
    return v.to_bytes(6, "big")


def itch_hdr(t, locate=LOCATE):
    # Message Type(1) Stock Locate(2) Tracking Number(2) Timestamp(6)
    return t.encode() + struct.pack(">HH", locate, TRACKING) + ts48(TS)


def p4(decimal_str):
    whole, _, frac = decimal_str.partition(".")
    frac = (frac + "0000")[:4]
    return int(whole) * 10_000 + int(frac)


def p8(decimal_str):
    whole, _, frac = decimal_str.partition(".")
    frac = (frac + "00000000")[:8]
    return int(whole) * 100_000_000 + int(frac)


def itch_messages():
    stock = alpha("AAPL", 8)
    m = []
    m.append(("S_system_event", itch_hdr("S", 0) + b"O"))
    m.append(("R_stock_directory", itch_hdr("R") + stock + b"QN" + struct.pack(">I", 100) + b"NC" +
              b"Z " + b"PNN1N" + struct.pack(">I", 0) + b"N"))
    m.append(("H_stock_trading_action", itch_hdr("H") + stock + b"T" + b" " + alpha("", 4)))
    m.append(("Y_reg_sho_restriction", itch_hdr("Y") + stock + b"0"))
    m.append(("L_market_participant_position", itch_hdr("L") + b"NSDQ" + stock + b"YNA"))
    m.append(("V_mwcb_decline_level", itch_hdr("V", 0) +
              struct.pack(">QQQ", p8("3000.12345678"), p8("2800.5"), p8("2600.00000001"))))
    m.append(("W_mwcb_status", itch_hdr("W", 0) + b"1"))
    m.append(("K_ipo_quoting_period_update", itch_hdr("K") + stock + struct.pack(">I", 34_200) + b"A" +
              struct.pack(">I", p4("12.34"))))
    m.append(("J_luld_auction_collar", itch_hdr("J") + stock +
              struct.pack(">IIII", p4("150"), p4("165"), p4("135"), 1)))
    m.append(("h_operational_halt", itch_hdr("h") + stock + b"QH"))
    m.append(("A_add_order", itch_hdr("A") + struct.pack(">Q", 1001) + b"B" + struct.pack(">I", 300) + stock +
              struct.pack(">I", p4("189.1234"))))
    m.append(("F_add_order_mpid", itch_hdr("F") + struct.pack(">Q", 1002) + b"S" + struct.pack(">I", 200) +
              stock + struct.pack(">I", p4("189.25")) + b"GSCO"))
    m.append(("E_order_executed", itch_hdr("E") + struct.pack(">QIQ", 1001, 100, 5001)))
    m.append(("C_order_executed_with_price", itch_hdr("C") + struct.pack(">QIQ", 1002, 50, 5002) + b"Y" +
              struct.pack(">I", p4("189.24"))))
    m.append(("X_order_cancel", itch_hdr("X") + struct.pack(">QI", 1001, 25)))
    m.append(("D_order_delete", itch_hdr("D") + struct.pack(">Q", 1002)))
    m.append(("U_order_replace", itch_hdr("U") + struct.pack(">QQII", 1001, 1003, 400, p4("189.10"))))
    m.append(("P_trade", itch_hdr("P") + struct.pack(">Q", 0) + b"B" + struct.pack(">I", 10) + stock +
              struct.pack(">IQ", p4("189.15"), 5003)))
    m.append(("Q_cross_trade", itch_hdr("Q") + struct.pack(">Q", 123_456) + stock +
              struct.pack(">IQ", p4("189.20"), 5004) + b"O"))
    m.append(("B_broken_trade", itch_hdr("B") + struct.pack(">Q", 5003)))
    m.append(("I_noii", itch_hdr("I") + struct.pack(">QQ", 1000, 500) + b"B" + stock +
              struct.pack(">III", p4("189"), p4("189.1"), p4("189.05")) + b"OL"))
    m.append(("N_retail_interest", itch_hdr("N") + stock + b"A"))
    m.append(("O_dlcr_price_discovery", itch_hdr("O") + stock + b"Y" +
              struct.pack(">III", p4("10"), p4("20"), p4("15")) + struct.pack(">Q", 34_200_000_000_000) +
              struct.pack(">II", p4("13.5"), p4("16.5"))))
    lengths = {"S": 12, "R": 39, "H": 25, "Y": 20, "L": 26, "V": 35, "W": 12, "K": 28, "J": 35, "h": 21,
               "A": 36, "F": 40, "E": 31, "C": 36, "X": 23, "D": 19, "U": 35, "P": 44, "Q": 40, "B": 19,
               "I": 50, "N": 20, "O": 48}
    for name, raw in m:
        assert len(raw) == lengths[name[0]], (name, len(raw))
    return m


TOKEN1 = alpha("fm000100000001", 14)
TOKEN2 = alpha("fm000100000002", 14)
TOKEN3 = alpha("fm000100000003", 14)
OUCH_TS = 34_200_000_000_456


def ouch42_messages():
    stock = alpha("AAPL", 8)
    q = lambda v: struct.pack(">Q", v)
    m = []
    # inbound
    m.append(("O_enter_order", b"O" + TOKEN1 + b"B" + struct.pack(">I", 100) + stock +
              struct.pack(">II", p4("189.1234"), 99_999) + alpha("", 4) + b"YPN" + struct.pack(">I", 0) + b"N "))
    m.append(("O_enter_order_fok_post_only", b"O" + TOKEN2 + b"S" + struct.pack(">I", 50) + stock +
              struct.pack(">II", p4("189.25"), 0) + alpha("", 4) + b"PPN" + struct.pack(">I", 50) + b"N "))
    m.append(("U_replace_order", b"U" + TOKEN1 + TOKEN3 + struct.pack(">III", 150, p4("189.20"), 99_999) + b"YN" +
              struct.pack(">I", 0)))
    m.append(("X_cancel_order", b"X" + TOKEN3 + struct.pack(">I", 0)))
    m.append(("M_modify_order", b"M" + TOKEN3 + b"S" + struct.pack(">I", 100)))
    # outbound
    m.append(("S_system_event", b"S" + q(OUCH_TS) + b"S"))
    m.append(("A_accepted", b"A" + q(OUCH_TS) + TOKEN1 + b"B" + struct.pack(">I", 100) + stock +
              struct.pack(">II", p4("189.1234"), 99_999) + alpha("", 4) + b"Y" + q(7777) + b"PN" +
              struct.pack(">I", 0) + b"NL "))
    m.append(("U_replaced", b"U" + q(OUCH_TS) + TOKEN3 + b"B" + struct.pack(">I", 150) + stock +
              struct.pack(">II", p4("189.20"), 99_999) + alpha("", 4) + b"Y" + q(7778) + b"PN" +
              struct.pack(">I", 0) + b"NL" + TOKEN1 + b" "))
    m.append(("E_executed", b"E" + q(OUCH_TS) + TOKEN3 + struct.pack(">II", 40, p4("189.20")) + b"A" + q(9001)))
    m.append(("C_canceled", b"C" + q(OUCH_TS) + TOKEN3 + struct.pack(">I", 110) + b"U"))
    m.append(("D_aiq_canceled", b"D" + q(OUCH_TS) + TOKEN3 + struct.pack(">I", 10) + b"Q" +
              struct.pack(">II", 10, p4("189.20")) + b"RD"))
    m.append(("B_broken_trade", b"B" + q(OUCH_TS) + TOKEN3 + q(9001) + b"E"))
    m.append(("G_executed_with_reference_price", b"G" + q(OUCH_TS) + TOKEN3 +
              struct.pack(">II", 5, p4("189.20")) + b"R" + q(9002) + struct.pack(">I", p4("189.19")) + b"I"))
    m.append(("J_rejected", b"J" + q(OUCH_TS) + TOKEN2 + b"X"))
    m.append(("P_cancel_pending", b"P" + q(OUCH_TS) + TOKEN3))
    m.append(("I_cancel_reject", b"I" + q(OUCH_TS) + TOKEN3))
    m.append(("T_order_priority_update", b"T" + q(OUCH_TS) + TOKEN3 + struct.pack(">I", p4("189.20")) + b"Y" +
              q(7779)))
    m.append(("M_order_modified", b"M" + q(OUCH_TS) + TOKEN3 + b"S" + struct.pack(">I", 100)))
    lengths = {"O_enter_order": 49, "O_enter_order_fok_post_only": 49, "U_replace_order": 47,
               "X_cancel_order": 19, "M_modify_order": 20, "S_system_event": 10, "A_accepted": 66,
               "U_replaced": 80, "E_executed": 40, "C_canceled": 28, "D_aiq_canceled": 38,
               "B_broken_trade": 32, "G_executed_with_reference_price": 45, "J_rejected": 24,
               "P_cancel_pending": 23, "I_cancel_reject": 23, "T_order_priority_update": 36,
               "M_order_modified": 28}
    for name, raw in m:
        assert len(raw) == lengths[name], (name, len(raw))
    return m


def ouch50_messages():
    sym = alpha("AAPL", 8)
    q = lambda v: struct.pack(">Q", v)
    i = lambda v: struct.pack(">I", v)
    h = lambda v: struct.pack(">H", v)
    m = []
    # inbound
    m.append(("O_enter_order", b"O" + i(1) + b"B" + i(100) + sym + q(p4("189.1234")) + b"0YPNN" + TOKEN1 + h(0)))
    minqty = bytes([5, 3]) + i(50)       # TagValue: Length 5 | MinQty(3) | 4-byte integer
    postonly = bytes([2, 12]) + b"P"     # TagValue: Length 2 | PostOnly(12) | 'P'
    m.append(("O_enter_order_fok_post_only", b"O" + i(2) + b"S" + i(50) + sym + q(p4("189.25")) + b"3YPNN" +
              TOKEN2 + h(len(minqty + postonly)) + minqty + postonly))
    m.append(("U_replace_order", b"U" + i(1) + i(3) + i(150) + q(p4("189.20")) + b"0YN" + TOKEN3 + h(0)))
    m.append(("X_cancel_order", b"X" + i(3) + i(0) + h(0)))
    # outbound
    m.append(("S_system_event", b"S" + q(OUCH_TS) + b"S"))
    m.append(("A_accepted", b"A" + q(OUCH_TS) + i(1) + b"B" + i(100) + sym + q(p4("189.1234")) + b"0Y" + q(7777) +
              b"PNNL" + TOKEN1 + h(0)))
    user_ref_idx = bytes([2, 28, 5])     # TagValue: Length 2 | UserRefIdx(28) | 5
    m.append(("A_accepted_with_appendage", b"A" + q(OUCH_TS) + i(2) + b"S" + i(50) + sym + q(p4("189.25")) +
              b"3Y" + q(7780) + b"PNNL" + TOKEN2 + h(len(user_ref_idx)) + user_ref_idx))
    m.append(("U_replaced", b"U" + q(OUCH_TS) + i(1) + i(3) + b"B" + i(150) + sym + q(p4("189.20")) + b"0Y" +
              q(7778) + b"PNNL" + TOKEN3 + h(0)))
    m.append(("E_executed", b"E" + q(OUCH_TS) + i(3) + i(40) + q(p4("189.20")) + b"R" + q(9001) + h(0)))
    m.append(("C_canceled", b"C" + q(OUCH_TS) + i(3) + i(100) + b"U" + h(0)))
    m.append(("C_canceled_no_appendage", b"C" + q(OUCH_TS) + i(3) + i(10) + b"U"))
    m.append(("J_rejected", b"J" + q(OUCH_TS) + i(4) + h(0x001D) + alpha("fm000100000004", 14) + h(0)))
    m.append(("I_cancel_reject", b"I" + q(OUCH_TS) + i(3) + h(0)))
    m.append(("Q_account_query_response", b"Q" + q(OUCH_TS) + i(42) + h(0)))
    m.append(("D_aiq_canceled", b"D" + q(OUCH_TS) + i(3) + i(10) + b"Q" + i(10) + q(p4("189.20")) + b"RD" + h(0)))
    m.append(("B_broken_trade", b"B" + q(OUCH_TS) + i(3) + q(9001) + b"E" + TOKEN3 + h(0)))
    m.append(("T_order_priority_update", b"T" + q(OUCH_TS) + i(3) + q(p4("189.20")) + b"Y" + q(7779) + h(0)))
    m.append(("M_order_modified", b"M" + q(OUCH_TS) + i(3) + b"S" + i(100) + h(0)))
    m.append(("R_order_restated", b"R" + q(OUCH_TS) + i(3) + b"R" + h(0)))
    lengths = {"O_enter_order": 47, "O_enter_order_fok_post_only": 47 + 9, "U_replace_order": 40,
               "X_cancel_order": 11, "S_system_event": 10, "A_accepted": 64, "A_accepted_with_appendage": 67,
               "U_replaced": 68, "E_executed": 36, "C_canceled": 20, "C_canceled_no_appendage": 18,
               "J_rejected": 31, "I_cancel_reject": 15, "Q_account_query_response": 15, "D_aiq_canceled": 34,
               "B_broken_trade": 38, "T_order_priority_update": 32, "M_order_modified": 20,
               "R_order_restated": 16}
    for name, raw in m:
        assert len(raw) == lengths[name], (name, len(raw))
    return m


def soup(t, payload=b""):
    return struct.pack(">H", len(payload) + 1) + t.encode() + payload


def soupbin_packets():
    m = []
    base_login = alpha("FMUSER", 6) + alpha("secret", 10) + alpha("", 10) + left_padded("1", 20)
    m.append(("L_login_request_v3", soup("L", base_login)))
    m.append(("L_login_request_v41", soup("L", base_login + left_padded("15000", 5))))
    m.append(("A_login_accepted", soup("A", left_padded("SESS01", 10) + left_padded("42", 20))))
    m.append(("J_login_rejected", soup("J", b"A")))
    m.append(("S_sequenced_data", soup("S", b"hello")))
    m.append(("U_unsequenced_data", soup("U", b"world")))
    m.append(("H_server_heartbeat", soup("H")))
    m.append(("R_client_heartbeat", soup("R")))
    m.append(("Z_end_of_session", soup("Z")))
    m.append(("O_logout_request", soup("O")))
    m.append(("plus_debug", soup("+", b"dbg")))
    assert len(m[0][1]) == 49 and len(m[1][1]) == 54 and len(m[2][1]) == 33 and len(m[3][1]) == 4
    return m


def moldudp64_packets():
    session = alpha("SESSION001", 10)
    m = []
    blocks = struct.pack(">H", 3) + b"abc" + struct.pack(">H", 0)
    m.append(("data_seq5_two_messages", session + struct.pack(">QH", 5, 2) + blocks))
    m.append(("heartbeat_next7", session + struct.pack(">QH", 7, 0)))
    m.append(("end_of_session_next7", session + struct.pack(">QH", 7, 0xFFFF)))
    m.append(("request_seq3_count4", session + struct.pack(">QH", 3, 4)))
    return m


def write(name, header, messages):
    path = os.path.join(HERE, name)
    with open(path, "w", encoding="ascii") as f:
        f.write("# Generated by generate_fixtures.py - do not edit.\n")
        f.write("# " + header + "\n")
        for key, raw in messages:
            f.write(f"{key} {raw.hex()}\n")


if __name__ == "__main__":
    write("itch50_messages.hex", "TotalView-ITCH 5.0, one message per line (all 23 types)", itch_messages())
    write("ouch42_messages.hex", "OUCH 4.2 inbound and outbound messages", ouch42_messages())
    write("ouch50_messages.hex", "OUCH 5.0 inbound and outbound messages", ouch50_messages())
    write("soupbin_packets.hex", "SoupBinTCP 3.00 / 4.00 / 4.10 logical packets", soupbin_packets())
    write("moldudp64_packets.hex", "MoldUDP64 1.00 downstream and request packets", moldudp64_packets())
