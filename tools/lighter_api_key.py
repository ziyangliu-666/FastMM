#!/usr/bin/env python3
"""Makes a Lighter API key and registers it in a slot of an account (ChangePubKey).

The account's Ethereum key signs the registration (an EIP-191 message naming the new public key,
the slot and the nonce); the new key's private half is printed once, for FASTMM_LIGHTER_API_KEY.
The Ethereum key is read from the environment variable --eth-key-env names, never from the command
line. Without --send nothing is sent: the script prints the account, the slot, its current key and
the new public key.

Needs the official SDK and eth-account (pip install lighter-sdk eth-account); runs anywhere the
trading host can reach the venue.

usage:
  LIGHTER_ETH_PRIVATE_KEY=0x... python3 tools/lighter_api_key.py --network testnet --slot 4
  LIGHTER_ETH_PRIVATE_KEY=0x... python3 tools/lighter_api_key.py --network mainnet --slot 4 --send
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
import urllib.request

HOSTS = {
    "mainnet": ("https://mainnet.zklighter.elliot.ai", 304),
    "testnet": ("https://testnet.zklighter.elliot.ai", 300),
}


def get(url: str) -> dict:
    with urllib.request.urlopen(url, timeout=10) as r:
        return json.loads(r.read())


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--network", choices=sorted(HOSTS), required=True)
    ap.add_argument("--slot", type=int, required=True, help="API key slot, 4 to 254 (0-3 are the apps')")
    ap.add_argument("--account", type=int, help="account index (default: the address's master account)")
    ap.add_argument("--eth-key-env", default="LIGHTER_ETH_PRIVATE_KEY",
                    help="environment variable holding the account's Ethereum private key")
    ap.add_argument("--send", action="store_true", help="register the key (otherwise only print the plan)")
    args = ap.parse_args()
    if not 4 <= args.slot <= 254:
        ap.error("--slot: 4 to 254")
    eth_key = os.environ.get(args.eth_key_env, "")
    if not eth_key:
        ap.error(f"set {args.eth_key_env} to the account's Ethereum private key")

    import eth_account
    import lighter

    url, chain_id = HOSTS[args.network]
    address = eth_account.Account.from_key(eth_key).address
    account = args.account
    if account is None:
        subs = get(f"{url}/api/v1/accountsByL1Address?l1_address={address}").get("sub_accounts", [])
        if not subs:
            print(f"no account for {address} on {args.network}: deposit first", file=sys.stderr)
            return 2
        account = min(int(s["index"]) for s in subs)
    keys = get(f"{url}/api/v1/apikeys?account_index={account}&api_key_index={args.slot}").get("api_keys", [])
    current = keys[0]["public_key"] if keys else "(empty)"

    private_key, public_key, err = lighter.create_api_key()
    if err:
        print(f"create_api_key: {err}", file=sys.stderr)
        return 1
    print(f"network {args.network}, address {address}, account {account}, slot {args.slot}")
    print(f"current public key: {current}")
    print(f"new public key:     {public_key}")
    if not args.send:
        print("not sent (--send registers it)")
        return 0
    client = lighter.SignerClient(url=url, account_index=account,
                                  api_private_keys={args.slot: private_key}, chain_id=chain_id)
    try:
        response, err = await client.change_api_key(eth_private_key=eth_key, new_pubkey=public_key,
                                                    api_key_index=args.slot)
        if err is not None:
            print(f"refused: {err}", file=sys.stderr)
            return 1
        print(f"sent: {response}")
        await asyncio.sleep(10)  # until the venue lists it
        err = client.check_client()
        if err is not None:
            print(f"not registered yet: {err}", file=sys.stderr)
            return 1
    finally:
        await client.close()
    print(f"registered. FASTMM_LIGHTER_ACCOUNT={account} api_key_index = {args.slot}")
    print(f"FASTMM_LIGHTER_API_KEY={private_key}")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
