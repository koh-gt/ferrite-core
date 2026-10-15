#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Read-only scan of Ferrite's MWEB history against the MWEB rules added in v3.2.1.

For every block from MWEB activation to the tip it checks:
  M1  each MWEB input's commitment and output pubkey match the output it spends
      (Litecoin e7cbf1d56; Ferrite grandfathers no block, so this must hold everywhere)
  M2  kernel lock_height <= block height, for blocks above 903,321
      (Litecoin a54900438; Ferrite KERNEL_LOCK_HEIGHT_GRANDFATHER_HEIGHT)
  X1  kernels that set the extra-data feature carry non-empty extra data
      (Litecoin a399fc4ca; only enforced from mweb_extradata_feature_activation_height)
  P1  kernels that set the pegout feature carry a pegout (Litecoin b250b016a; only enforced
      from mweb_pegout_feature_activation_height). RPC does not show a kernel's pegouts, so
      this is a lower bound: a block is reported when it has more pegout-feature kernels than
      HogEx pegout outputs.
M1 and M2 apply to past blocks: a violation means v3.2.1 would reject that block.
X1 and P1 only start at a future height; past violations are reported for information.

The script only calls getblockchaininfo, getblockhash and getblock. It changes nothing.

Usage (Python 3.8+, standard library only), on the machine running ferrited / ferrite-qt (server=1):
  python3 mweb_history_check.py                          # reads the .cookie from the default datadir
  python3 mweb_history_check.py --datadir /path/to/datadir
  python  mweb_history_check.py --rpcuser USER --rpcpassword PASS
"""
import argparse
import base64
import json
import os
import sys
import time
import urllib.error
import urllib.request

LOCK_HEIGHT_GRANDFATHER = 903_321
PEGOUT_FEATURE_BIT = 0x04
EXTRA_DATA_FEATURE_BIT = 0x20


def default_datadir():
    if sys.platform == "win32":
        return os.path.join(os.environ.get("APPDATA", ""), "Ferrite")
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Application Support/Ferrite")
    return os.path.expanduser("~/.ferrite")


class Rpc:
    def __init__(self, url, user, password):
        self.url = url
        token = base64.b64encode(f"{user}:{password}".encode()).decode()
        self.headers = {"Authorization": "Basic " + token, "Content-Type": "application/json"}

    def batch(self, calls):
        body = json.dumps([{"jsonrpc": "1.0", "id": i, "method": m, "params": p}
                           for i, (m, p) in enumerate(calls)]).encode()
        for attempt in range(5):
            try:
                req = urllib.request.Request(self.url, data=body, headers=self.headers)
                with urllib.request.urlopen(req, timeout=600) as resp:
                    replies = json.loads(resp.read())
                break
            except urllib.error.HTTPError as e:
                if e.code == 401:
                    sys.exit("RPC authentication failed (HTTP 401): check --datadir or --rpcuser/--rpcpassword.")
                if attempt == 4:
                    raise
            except (urllib.error.URLError, ConnectionError, TimeoutError):
                if attempt == 4:
                    raise
            time.sleep(2 ** attempt)
        replies.sort(key=lambda r: r["id"])
        for r in replies:
            if r.get("error"):
                raise RuntimeError(f"RPC error: {r['error']}")
        return [r["result"] for r in replies]

    def call(self, method, *params):
        return self.batch([(method, list(params))])[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rpcurl", default="http://127.0.0.1:9573/", help="default: %(default)s (mainnet)")
    ap.add_argument("--datadir", default=default_datadir(), help="used to read the .cookie file (default: %(default)s)")
    ap.add_argument("--rpcuser")
    ap.add_argument("--rpcpassword")
    ap.add_argument("--start", type=int, help="first height (default: MWEB activation height; "
                    "a later start cannot check M1 for outputs created before it)")
    ap.add_argument("--end", type=int, help="last height (default: tip)")
    ap.add_argument("--batch", type=int, default=250)
    args = ap.parse_args()

    if args.rpcuser and args.rpcpassword:
        user, password = args.rpcuser, args.rpcpassword
    else:
        cookie = os.path.join(args.datadir, ".cookie")
        try:
            with open(cookie) as f:
                user, password = f.read().strip().split(":", 1)
        except OSError:
            sys.exit(f"Cannot read {cookie}; pass --datadir or --rpcuser/--rpcpassword.")
    rpc = Rpc(args.rpcurl, user, password)

    info = rpc.call("getblockchaininfo")
    mweb = info.get("softforks", {}).get("mweb", {})
    activation = mweb.get("height")
    print(f"chain={info['chain']} blocks={info['blocks']} headers={info['headers']} "
          f"mweb_active={mweb.get('active')} mweb_height={activation}")
    if activation is None:
        sys.exit("MWEB is not active on this chain; nothing to check.")
    start = max(args.start if args.start is not None else activation, activation)
    end = args.end if args.end is not None else info["blocks"]
    complete_history = start == activation

    outputs = {}  # unspent MWEB output_id -> (commit, receiver_pubkey)
    counts = dict(blocks=0, mweb_blocks_with_data=0, inputs=0, inputs_not_checked=0, outputs=0,
                  kernels=0, pegout_kernels=0, hogex_pegouts=0, extradata_kernels=0, max_lock_height=0)
    problems, notes = [], []
    t0 = time.time()

    for lo in range(start, end + 1, args.batch):
        hi = min(lo + args.batch - 1, end)
        hashes = rpc.batch([("getblockhash", [h]) for h in range(lo, hi + 1)])
        summaries = rpc.batch([("getblock", [bh, 1]) for bh in hashes])
        busy = [(lo + i, bh) for i, (bh, b) in enumerate(zip(hashes, summaries))
                if b.get("mweb") and (b["mweb"]["inputs"] or b["mweb"]["outputs"] or b["mweb"]["kernels"])]
        counts["blocks"] += len(hashes)
        details = rpc.batch([("getblock", [bh, 2]) for _, bh in busy]) if busy else []
        for (height, bh), block in zip(busy, details):
            mw = block["mweb"]
            counts["mweb_blocks_with_data"] += 1
            # Consensus order (mw::CoinsViewCache::ApplyBlock): add the block's outputs, then spend its inputs.
            for out in mw["outputs"]:
                outputs[out["output_id"]] = (out["commit"], out["receiver_pubkey"])
                counts["outputs"] += 1
            for inp in mw["inputs"]:
                counts["inputs"] += 1
                spent = outputs.pop(inp["output_id"], None)
                if spent is None:
                    if complete_history:
                        problems.append(f"M1 {height} {bh}: input spends unknown output {inp['output_id']}")
                    else:
                        counts["inputs_not_checked"] += 1
                elif spent != (inp["commit"], inp["output_pubkey"]):
                    problems.append(f"M1 {height} {bh}: input {inp['output_id']} commitment/pubkey differ from the spent output")
            pegout_kernels = 0
            for k in mw["kernels"]:
                counts["kernels"] += 1
                counts["max_lock_height"] = max(counts["max_lock_height"], k["lock_height"])
                if k["features"] & PEGOUT_FEATURE_BIT:
                    pegout_kernels += 1
                if k["features"] & EXTRA_DATA_FEATURE_BIT:
                    counts["extradata_kernels"] += 1
                    if not k.get("extra_data"):
                        notes.append(f"X1 {height} {bh}: kernel {k['kernel_id']} sets the extra-data feature with empty data")
                if height > LOCK_HEIGHT_GRANDFATHER and k["lock_height"] > height:
                    problems.append(f"M2 {height} {bh}: kernel {k['kernel_id']} lock_height {k['lock_height']} > height")
            hogex_pegouts = len(block["tx"][-1]["vout"]) - 1  # HogEx vout[0] is the HogAddr output
            counts["pegout_kernels"] += pegout_kernels
            counts["hogex_pegouts"] += hogex_pegouts
            if pegout_kernels > hogex_pegouts:
                notes.append(f"P1 {height} {bh}: {pegout_kernels} pegout-feature kernels but {hogex_pegouts} HogEx pegouts")
        done = hi - start + 1
        rate = done / max(time.time() - t0, 1e-6)
        print(f"\r  {hi}/{end}  {rate:,.0f} blocks/s  MWEB blocks with data: {counts['mweb_blocks_with_data']}  "
              f"M1/M2 problems: {len(problems)}   ", end="", flush=True)

    print(f"\n\nScanned heights {start} to {end} ({'complete MWEB history' if complete_history else 'partial range'})")
    for key, value in counts.items():
        print(f"  {key:22} {value:,}")
    print(f"  {'unspent_mweb_outputs':22} {len(outputs):,}" + ("" if complete_history else " (in range)"))
    if notes:
        print(f"\n{len(notes)} note(s) on rules that only start at a future height (X1/P1):")
        for line in notes[:50]:
            print("  " + line)
        if len(notes) > 50:
            print(f"  ... and {len(notes) - 50} more")
    if problems:
        print(f"\n{len(problems)} problem(s) with rules that apply to past blocks (M1/M2):")
        for line in problems:
            print("  " + line)
        sys.exit(1)
    print("\nRESULT: no M1/M2 violations" + ("" if complete_history else " in the scanned range")
          + f" (heights {start}-{end}).")


if __name__ == "__main__":
    main()
