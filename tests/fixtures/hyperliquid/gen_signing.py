"""Golden vectors for the Hyperliquid L1 action signing, from hyperliquid-python-sdk (0.24.0):
    pip install hyperliquid-python-sdk==0.24.0 && python gen_signing.py > signing_golden.json
tests/venues/hyperliquid_signing_test.cpp carries the same cases. The key is a throwaway test key
that controls no account."""
import json, sys
import msgpack
from eth_account import Account
from eth_account.messages import encode_typed_data, _hash_eip191_message
from hyperliquid.utils.signing import (action_hash, construct_phantom_agent, l1_payload, sign_l1_action,
                                       order_request_to_order_wire, order_wires_to_order_action, float_to_wire)
from hyperliquid.utils.types import Cloid

KEY = "0x4c0883a69102937d6231471b5dbb6204fe5129617082792ae468d01a3f362318"
w = Account.from_key(KEY)
NVDA = 110002
BTC = 0
cl1 = Cloid.from_str("0x666d00000000000000010000002a0005")
cl2 = Cloid.from_str("0x666d000000000000000100000000ffff")


def wire(coin_asset, is_buy, sz, px, tif, reduce_only=False, cloid=None):
    o = {"coin": "x", "is_buy": is_buy, "sz": sz, "limit_px": px, "order_type": {"limit": {"tif": tif}},
         "reduce_only": reduce_only}
    if cloid is not None:
        o["cloid"] = cloid
    return order_request_to_order_wire(o, coin_asset)


cases = []


def case(name, action, nonce, vault=None, expires=None, mainnet=True):
    h = action_hash(action, vault, nonce, expires)
    data = l1_payload(construct_phantom_agent(h, mainnet))
    digest = _hash_eip191_message(encode_typed_data(full_message=data))
    sig = sign_l1_action(w, action, vault, nonce, expires, mainnet)
    payload = {"action": action, "nonce": nonce, "signature": sig, "vaultAddress": vault, "expiresAfter": expires}
    cases.append({"name": name, "nonce": nonce, "mainnet": mainnet, "vault": vault, "expires_after": expires,
                  "msgpack": msgpack.packb(action).hex(), "connection_id": h.hex(), "digest": digest.hex(),
                  "r": sig["r"], "s": sig["s"], "v": sig["v"],
                  "payload": json.dumps(payload, separators=(",", ":"))})


case("order_alo_cloid", order_wires_to_order_action([wire(NVDA, True, 0.05, 230.32, "Alo", cloid=cl1)]),
     1791781700000)
case("order_gtc_reduce_only", order_wires_to_order_action([wire(BTC, False, 0.001, 86287.0, "Gtc", True)]),
     1791781700001)
case("order_batch_two", order_wires_to_order_action(
    [wire(NVDA, True, 0.05, 230.3, "Alo", cloid=cl1), wire(NVDA, False, 1.234, 230.41, "Alo", cloid=cl2)]),
     1791781700002)
case("order_ioc", order_wires_to_order_action([wire(NVDA, True, 12.5, 231.0, "Ioc", cloid=cl2)]), 1791781700003)
case("cancel_two", {"type": "cancel", "cancels": [{"a": NVDA, "o": 571712108382}, {"a": BTC, "o": 1}]},
     1791781700004)
case("cancel_by_cloid", {"type": "cancelByCloid", "cancels": [{"asset": NVDA, "cloid": cl1.to_raw()}]},
     1791781700005)
case("batch_modify_oid", {"type": "batchModify", "modifies": [
    {"oid": 571712108382, "order": wire(NVDA, True, 0.05, 230.25, "Alo", cloid=cl2)}]}, 1791781700006)
case("batch_modify_cloid", {"type": "batchModify", "modifies": [
    {"oid": cl1.to_raw(), "order": wire(NVDA, False, 0.07, 230.5, "Alo", cloid=cl2)}]}, 1791781700007)
case("schedule_cancel_time", {"type": "scheduleCancel", "time": 1791781760000}, 1791781700008)
case("schedule_cancel_clear", {"type": "scheduleCancel"}, 1791781700009)
case("order_testnet", order_wires_to_order_action([wire(NVDA, True, 0.05, 230.32, "Alo", cloid=cl1)]),
     1791781700010, mainnet=False)
case("order_vault", order_wires_to_order_action([wire(NVDA, True, 0.05, 230.32, "Alo", cloid=cl1)]),
     1791781700011, vault="0x1719884eb866cb12b2287399b15f7db5e7d775ea")
case("order_expires", order_wires_to_order_action([wire(NVDA, True, 0.05, 230.32, "Alo", cloid=cl1)]),
     1791781700012, expires=1791781760000)
# Price and size texts the SDK puts on the wire.
texts = {str(x): float_to_wire(x) for x in [230.32, 230.3, 86287.0, 0.001, 12.5, 1.234, 0.05, 30958.0, 0.000123]}
json.dump({"key": KEY, "address": w.address.lower(), "cases": cases, "float_to_wire": texts}, sys.stdout, indent=1)
