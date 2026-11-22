#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Write the MWEB test vectors of the OP_RETURN record's unit tests.

Runs a regtest ferrited in a new temporary directory and activates MWEB. Around the activation it makes blocks with
a peg-in, a transaction inside MWEB, a peg-out, transactions with OP_RETURN outputs that spend legacy, P2SH-wrapped
segwit and native segwit coins, one with two OP_RETURN outputs, one with an MWEB part and an OP_RETURN output, and a
coinbase whose reward goes to an OP_RETURN output. It writes the raw blocks, their undo data as the node stored it,
and what the record must find in them (the rows, and for each transaction with rows its kind and the coins it
spends) to the output file. The wallet makes no peg-out to an OP_RETURN script, so no integrating transaction of
these blocks has a row; the unit tests give one such output to a copy of one.

Run it by hand when the record's format or these blocks change, and commit the result:

    contrib/devtools/gen-opreturn-mweb-vectors.py src/ferrited src/test/data/opreturn_mweb_blocks.json

The node listens only on 127.0.0.1, on the ports given (--port, --rpcport), and is stopped at the end.
"""

import argparse
import base64
import hashlib
import http.client
import json
import os
import shutil
import struct
import subprocess
import tempfile
import time

COMMITMENT_HEADER = "6a24aa21a9ed"


class Node:
    def __init__(self, ferrited, datadir, port, rpcport):
        self.datadir = datadir
        self.rpcport = rpcport
        self.process = subprocess.Popen([
            ferrited, "-regtest", "-datadir=" + datadir, "-port=%d" % port, "-rpcport=%d" % rpcport,
            "-rpcbind=127.0.0.1", "-rpcallowip=127.0.0.1", "-listen=0", "-dnsseed=0",
            "-txindex=1", "-acceptnonstdtxn=1", "-fallbackfee=0.0002", "-server=1",
            "-printtoconsole=0", "-whitelist=noban@127.0.0.1",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 60
        while True:
            try:
                self.call("getblockcount")
                return
            except Exception:
                if time.time() > deadline or self.process.poll() is not None:
                    raise RuntimeError("ferrited did not start")
                time.sleep(0.5)

    def call(self, method, *params, wallet=None):
        with open(os.path.join(self.datadir, "regtest", ".cookie"), encoding="ascii") as cookie:
            auth = base64.b64encode(cookie.read().strip().encode()).decode()
        connection = http.client.HTTPConnection("127.0.0.1", self.rpcport, timeout=120)
        path = "/wallet/" + wallet if wallet is not None else "/"
        body = json.dumps({"version": "1.1", "method": method, "params": list(params), "id": 1})
        connection.request("POST", path, body, {"Authorization": "Basic " + auth, "Content-Type": "application/json"})
        reply = json.loads(connection.getresponse().read().decode())
        connection.close()
        if reply.get("error"):
            raise RuntimeError("%s: %s" % (method, reply["error"]))
        return reply["result"]

    def stop(self):
        try:
            self.call("stop")
        except Exception:
            pass
        self.process.wait(timeout=120)


def amount_atoms(value):
    return int(round(value * 100000000))


def ser_compact(n):
    if n < 253:
        return bytes([n])
    if n <= 0xffff:
        return b"\xfd" + struct.pack("<H", n)
    if n <= 0xffffffff:
        return b"\xfe" + struct.pack("<I", n)
    return b"\xff" + struct.pack("<Q", n)


def raw_transaction(inputs, outputs):
    """An unsigned transaction without witnesses: inputs as (txid hex, vout), outputs as (atoms, script bytes)."""
    data = struct.pack("<i", 2) + ser_compact(len(inputs))
    for txid, vout in inputs:
        data += bytes.fromhex(txid)[::-1] + struct.pack("<I", vout) + ser_compact(0) + struct.pack("<I", 0xfffffffe)
    data += ser_compact(len(outputs))
    for atoms, script in outputs:
        data += struct.pack("<q", atoms) + ser_compact(len(script)) + script
    return (data + struct.pack("<I", 0)).hex()


def op_return(text):
    data = text.encode()
    assert len(data) < 76
    return bytes([0x6a, len(data)]) + data


def spend_with_op_returns(node, wallet, address, texts, fee=0.001):
    """Sends a transaction that spends the coin of `address` to OP_RETURN outputs and change."""
    coins = [c for c in node.call("listunspent", 1, 9999999, [address], wallet=wallet)]
    assert coins, "no coin at " + address
    coin = coins[0]
    change = node.call("getnewaddress", "", "bech32", wallet=wallet)
    change_script = bytes.fromhex(node.call("getaddressinfo", change, wallet=wallet)["scriptPubKey"])
    outputs = [(0, op_return(text)) for text in texts]
    outputs.append((amount_atoms(coin["amount"] - fee), change_script))
    unsigned = raw_transaction([(coin["txid"], coin["vout"])], outputs)
    signed = node.call("signrawtransactionwithwallet", unsigned, wallet=wallet)
    assert signed["complete"], signed
    return node.call("sendrawtransaction", signed["hex"], 0)


def read_undo(datadir, wanted):
    """The undo data of the blocks in `wanted` (block hash -> previous block hash), from the node's rev files."""
    found = {}
    blocks = os.path.join(datadir, "regtest", "blocks")
    for name in sorted(os.listdir(blocks)):
        if not (name.startswith("rev") and name.endswith(".dat")):
            continue
        path = os.path.join(blocks, name)
        with open(path, "rb") as f:
            data = f.read()
        pos = 0
        while pos + 8 <= len(data):
            magic = data[pos:pos + 4]
            if magic == b"\x00\x00\x00\x00":
                break
            size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
            body = data[pos + 8:pos + 8 + size]
            checksum = data[pos + 8 + size:pos + 8 + size + 32]
            pos += 8 + size + 32
            for block, prev in wanted.items():
                digest = hashlib.sha256(hashlib.sha256(bytes.fromhex(prev)[::-1] + body).digest()).digest()
                if digest == checksum:
                    found[block] = body.hex()
    return found


def coin_of(node, txid, vout):
    tx = node.call("getrawtransaction", txid, True)
    height = node.call("getblockheader", tx["blockhash"])["height"]
    out = tx["vout"][vout]
    return {
        "value": amount_atoms(out["value"]),
        "script": out["scriptPubKey"]["hex"],
        "height": height,
        "coinbase": "coinbase" in tx["vin"][0],
    }


def describe(node, block_hash, mweb_active):
    block = node.call("getblock", block_hash, 2)
    rows = []
    txs = []
    coinbase_outputs = block["tx"][0]["vout"]
    commitment = -1
    for i, out in enumerate(coinbase_outputs):
        script = out["scriptPubKey"]["hex"]
        if script.startswith(COMMITMENT_HEADER) and len(script) >= 76:
            commitment = i
    for pos, tx in enumerate(block["tx"]):
        tx_rows = []
        for i, out in enumerate(tx["vout"]):
            script = out["scriptPubKey"]["hex"]
            if pos == 0 and i == commitment:
                continue
            if script.startswith("6a"):
                tx_rows.append({"txpos": pos, "vout": i, "value": amount_atoms(out["value"]), "script": script})
        if not tx_rows:
            continue
        rows.extend(tx_rows)
        coinbase = "coinbase" in tx["vin"][0] if tx["vin"] else False
        hogex = mweb_active and pos == len(block["tx"]) - 1
        inputs = [] if coinbase else [coin_of(node, i["txid"], i["vout"]) for i in tx["vin"]]
        txs.append({
            "txpos": pos,
            "txid": tx["txid"],
            "coinbase": coinbase,
            "hogex": hogex,
            "pegin": any(o["scriptPubKey"]["hex"].startswith("5920") and len(o["scriptPubKey"]["hex"]) == 68 for o in tx["vout"]),
            "inputs": inputs,
        })
    return block, rows, txs


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("ferrited", help="the ferrited program to run")
    parser.add_argument("output", help="the JSON file to write")
    parser.add_argument("--port", type=int, default=55471)
    parser.add_argument("--rpcport", type=int, default=55472)
    parser.add_argument("--tmpdir", help="where the node's temporary data directory goes (default: the system's)")
    args = parser.parse_args()

    datadir = tempfile.mkdtemp(prefix="opreturn-mweb-vectors-", dir=args.tmpdir)
    node = Node(args.ferrited, datadir, args.port, args.rpcport)
    try:
        node.call("createwallet", "w")
        node.call("createwallet", "m")
        mine = node.call("getnewaddress", "", "bech32", wallet="w")
        node.call("generatetoaddress", 120, mine)
        legacy = node.call("getnewaddress", "", "legacy", wallet="w")
        wrapped = node.call("getnewaddress", "", "p2sh-segwit", wallet="w")
        native = node.call("getnewaddress", "", "bech32", wallet="w")
        two = node.call("getnewaddress", "", "bech32", wallet="w")
        for address in (legacy, wrapped, native, two):
            node.call("sendtoaddress", address, 5, wallet="w")
        node.call("generatetoaddress", 429 - node.call("getblockcount"), mine)
        first = node.call("getblockcount") + 1  # the first block of the vectors, before MWEB

        # Before MWEB: a transaction with an OP_RETURN output spending a legacy coin.
        spend_with_op_returns(node, "w", legacy, ["before mweb"])
        node.call("generatetoaddress", 2, mine)  # up to the last block without MWEB

        # The first MWEB block: a peg-in, and OP_RETURN outputs spending wrapped and native segwit coins, two in one.
        mweb_w = node.call("getnewaddress", "", "mweb", wallet="w")
        node.call("sendtoaddress", mweb_w, 20, wallet="w")
        spend_with_op_returns(node, "w", wrapped, ["wrapped segwit"])
        spend_with_op_returns(node, "w", native, ["native segwit"])
        spend_with_op_returns(node, "w", two, ["first of two", "second of two"])
        node.call("generatetoaddress", 1, mine)
        activation = node.call("getblockcount")

        # A transaction inside MWEB, and one with an MWEB part and an OP_RETURN output.
        mweb_m = node.call("getnewaddress", "", "mweb", wallet="m")
        node.call("sendtoaddress", mweb_m, 8, wallet="w")
        node.call("send", [{mweb_w: 1}, {"data": op_return("with an mweb part")[2:].hex()}], None, "unset", None,
                  {"add_to_wallet": True}, wallet="w")
        node.call("generatetoaddress", 1, mine)

        # A peg-out, from a wallet that holds only MWEB coins.
        node.call("sendtoaddress", node.call("getnewaddress", "", "bech32", wallet="w"), 2, wallet="m")
        node.call("generatetoaddress", 1, mine)

        # A coinbase whose reward goes to an OP_RETURN output.
        node.call("generatetodescriptor", 1, "raw(%s)" % op_return("coinbase reward").hex())
        node.call("generatetoaddress", 1, mine)
        last = node.call("getblockcount")

        entries = []
        wanted = {}
        for height in range(first, last + 1):
            block_hash = node.call("getblockhash", height)
            block, rows, txs = describe(node, block_hash, height >= activation)
            wanted[block_hash] = block["previousblockhash"]
            entries.append({
                "height": height,
                "hash": block_hash,
                "mweb": height >= activation,
                "hex": node.call("getblock", block_hash, 0),
                "rows": rows,
                "txs": txs,
            })
    finally:
        node.stop()
    undo = read_undo(datadir, wanted)
    shutil.rmtree(datadir)
    for entry in entries:
        entry["undo"] = undo[entry["hash"]]
    with open(args.output, "w", encoding="ascii") as out:
        json.dump({
            "comment": "Written by contrib/devtools/gen-opreturn-mweb-vectors.py; do not edit by hand.",
            "first_mweb_height": activation,
            "blocks": entries,
        }, out, indent=1, sort_keys=True)
        out.write("\n")
    print("%d blocks, %d rows" % (len(entries), sum(len(e["rows"]) for e in entries)))


if __name__ == "__main__":
    main()
