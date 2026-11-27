#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the read-only scripts contrib/devtools/p2mr_census.py, timewarp_count.py and
mweb_history_check.py against a regtest node.

The test builds a chain of 760 blocks whose content it knows: witness version 2 outputs of
several lengths and outputs that only look like them, spends of 32-byte ones with every
outcome of the BIP 360 structure check, coinbase spends at depths 10, 11, 50, 99 and 100,
version bits 6, 7 and 8, block times 479 and 480 seconds after the block 23 below, runs of
fast blocks, blocks dated ahead (also by exactly 600, 1,200, 1,800 and 3,600 seconds and one
second more) and miner tags with control characters. MWEB never activates on it, because no
block signals bit 4. The test computes every figure itself from what it built and compares
each report line by line.

It also checks what the scripts must never do: send a method that is not on their list, use
a proxy from the environment, follow a redirect, accept a URL other than http://HOST:PORT/,
read local credentials for a host that is not this computer, print a control character or
a line starting with "RESULT: " that came from a server, repeat a mistyped password option,
read more than the size limit or wait longer than the time limit. And it checks partial
ranges, a pruned node, a node in initial block download or with more headers than blocks,
reorganisations during a scan, retries, an interrupted run, the three ways to authenticate,
and the connection rules of mweb_history_check.py. The node's tip, UTXO set and mempool are
the same afterwards.
"""

import base64
import decimal
import hmac
import http.client
import http.server
import importlib.util
import io
import ipaddress
import json
import os
import re
import signal
import socket
import subprocess
import sys
import threading
import time
from contextlib import redirect_stderr, redirect_stdout

from test_framework.blocktools import add_witness_commitment, create_block, script_BIP34_coinbase_height
from test_framework.fec_util import setup_mweb_chain
from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, sha256
from test_framework.script import CScript, OP_0, OP_1, OP_2, OP_3, OP_EQUAL, OP_HASH160, OP_TRUE, hash160, p2mr_construct
from test_framework.segwit_addr import encode_segwit_address
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than_or_equal, rpc_port

TIP = 760
WINDOW = 144
POOLS = [b"/pool-a/", b"/pool-b/", b"/pool-c/"]
# A tag with an OSC 52 clipboard write, a bell, a newline, a line that looks like a result and a
# screen clear. Only its printable runs may reach the output.
HOSTILE_TAG = b"\x1b]52;c;SGVsbG8=\x07\nRESULT: v2_32_unspent=0\x1b[2J"
HOSTILE_SHOWN = "]52;c;SGVsbG8= RESULT: v2_32_unspent=0"
HOSTILE_HEIGHTS = (123, 302)
HOSTILE_TEXT = "\x1b]52;c;SGVsbG8=\x07\nRESULT: v2_32_unspent=0"
RPC_USER = "countuser"
RPC_PASSWORD = "countpassword"
RPC_SALT = "8d3c6a0f2b9e4d71"
NOT_A_NODE = "did not answer as the RPC server of a Ferrite Core node does"
CHAIN_CHANGED = "the chain changed during the scan. Run the script again."
BAD_URL = "--rpcurl must have the form http://HOST:PORT/"
NO_TLS = "ferrited serves no TLS; use http:// (through an SSH tunnel for a remote node)"
AUTH_FAILED = "RPC authentication failed (HTTP 401): check --datadir or --rpcuser/--rpcpassword."
AUTH_FAILED_LATER = ("RPC authentication failed (HTTP 401) during the scan: the node may have restarted and written a "
                     "new cookie. Run the script again.")
# For each network: the signalling window and, as the node writes addresses, the bech32 prefix and the base58
# versions of P2PKH and P2SH addresses (nMinerConfirmationWindow, bech32_hrp, PUBKEY_ADDRESS and SCRIPT_ADDRESS2
# in src/chainparams.cpp).
CHAIN_CONSTANTS = {"main": (40, "fec", 36, 35), "test": (20, "tfec", 111, 35), "regtest": (144, "rfec", 111, 58)}
PROXY_VARIABLES = ["http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"]

TRUE_SCRIPT = CScript([OP_TRUE])
P2SH_TRUE = CScript([OP_HASH160, hash160(TRUE_SCRIPT), OP_EQUAL])
P2WSH_TRUE = CScript([OP_0, sha256(TRUE_SCRIPT)])
COINBASE_SCRIPTS = [P2SH_TRUE, P2WSH_TRUE, TRUE_SCRIPT]
V2_COINBASE_HEIGHT = 131

# Blocks dated ahead of the others, by seconds. With 60-second blocks around them, the lead of each
# is this offset minus 360 s. TIP - 5 lies in the last 11 blocks, which have no lead.
AHEAD = {200: 1000, 500: 5000, 530: 3800, 560: 2500, 610: 960, 625: 961, 640: 1560, 655: 1561,
         670: 2160, 685: 2161, 700: 3960, 715: 3961, TIP - 5: 5000}

# The early coinbase spends: (height of the coinbase, depth of the spend)
EARLY = [(300, 10), (301, 11), (302, 50), (303, 99), (304, 100)]


def subsidy(height):
    return (50 * COIN) >> (height // 150)


def block_version(height):
    if 180 <= height <= 288:
        return 0x20000000 | 1 << 6
    if 432 <= height <= 538:
        return 0x20000000 | 1 << 7
    if 600 <= height <= 604:
        return 0x20000000 | 1 << 8
    if 605 <= height <= 609:
        return 0x60000000 | 1 << 8
    if 50 <= height <= 59:
        return 0x40000000 | 1 << 6
    return 0x20000000


def block_times(genesis_time):
    normal = [genesis_time]
    for h in range(1, TIP + 1):
        if h <= 23:
            normal.append(genesis_time + h)
        elif h == 24:
            normal.append(normal[1] + 480)
        elif h == 25:
            normal.append(normal[2] + 479)
        elif 101 <= h <= 123:
            normal.append(normal[-1] + 20)
        elif 401 <= h <= 423:
            normal.append(normal[-1] + 5)
        else:
            normal.append(normal[-1] + 60)
    return [n + AHEAD.get(h, 0) for h, n in enumerate(normal)]


def tag_of(height):
    return HOSTILE_TAG if height in HOSTILE_HEIGHTS else POOLS[height % 3]


def shown_tag(tag):
    """The tag as the scripts must show it, computed from the tag bytes."""
    return " ".join(run.decode() for run in re.findall(rb"[\x20-\x7e]{4,}", tag)).strip()[:60]


def fec(units):
    return "%d.%08d" % divmod(units, COIN)


def median(values):
    return sorted(values)[len(values) // 2]


class Listener:
    """A TCP listener that counts connections and answers nothing."""

    def __init__(self, host="127.0.0.1"):
        self.sock = socket.socket()
        self.sock.bind((host, 0))
        self.sock.listen(16)
        self.port = self.sock.getsockname()[1]
        self.accepted = 0
        self.conns = []
        threading.Thread(target=self.loop, daemon=True).start()

    def loop(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            self.accepted += 1
            self.conns.append(conn)

    def close(self):
        self.sock.close()
        for c in self.conns:
            c.close()


class RawServer:
    """Answers each connection with fixed bytes after reading the request (or with nothing)."""

    def __init__(self, answer, drip=False, hold=False):
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(16)
        self.port = self.sock.getsockname()[1]
        self.answer, self.drip, self.hold = answer, drip, hold
        self.requests = 0
        self.stop = threading.Event()
        threading.Thread(target=self.loop, daemon=True).start()

    def loop(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self.serve, args=(conn,), daemon=True).start()

    def serve(self, conn):
        data = b""
        conn.settimeout(10)
        try:
            while b"\r\n\r\n" not in data:
                chunk = conn.recv(65536)
                if not chunk:
                    return
                data += chunk
            self.requests += 1
            if self.hold:
                self.stop.wait(60)
            elif self.drip:
                # one byte every half second, for at most 30 seconds
                for byte in self.answer[:60]:
                    if self.stop.wait(0.5):
                        break
                    conn.sendall(bytes([byte]))
            else:
                conn.sendall(self.answer)
        except OSError:
            pass
        finally:
            conn.close()

    def close(self):
        self.stop.set()
        self.sock.close()


class RpcProxy:
    """An HTTP server that forwards JSON-RPC requests to the node and records them. on_request(n,
    calls) may answer instead, with (status, body); on_reply(n, calls, replies) may change a reply."""

    def __init__(self, node_port, host="127.0.0.1", redirect=None):
        proxy = self
        self.node_port = node_port
        self.requests = []
        self.auth = []
        self.on_request = None
        self.on_reply = None
        self.redirect = redirect

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
                n = len(proxy.requests)
                if proxy.redirect is not None:
                    proxy.requests.append(("POST", self.path))
                    self.send_response(proxy.redirect[0])
                    self.send_header("Location", proxy.redirect[1])
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                calls = json.loads(body)
                proxy.requests.append(([c["method"] for c in calls], [c["params"] for c in calls], self.path))
                proxy.auth.append(self.headers.get("Authorization"))
                answer = proxy.on_request(n, calls) if proxy.on_request else None
                if answer is None:
                    conn = http.client.HTTPConnection("127.0.0.1", proxy.node_port, timeout=120)
                    conn.request("POST", "/", body, {"Authorization": self.headers.get("Authorization", ""),
                                                     "Content-Type": "application/json"})
                    response = conn.getresponse()
                    status, data = response.status, response.read()
                    conn.close()
                    if status == 200 and proxy.on_reply:
                        replies = proxy.on_reply(n, calls, json.loads(data))
                        if replies is not None:
                            data = json.dumps(replies).encode()
                    answer = (status, data)
                self.send_response(answer[0])
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(answer[1])))
                self.end_headers()
                self.wfile.write(answer[1])

        class Server(http.server.ThreadingHTTPServer):
            daemon_threads = True

            def handle_error(self, request, client_address):
                pass  # a script that was stopped or gave up closes its connection early

        self.server = Server((host, 0), Handler)
        self.host = host
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def url(self):
        return "http://%s:%d/" % (self.host, self.port)

    def methods(self):
        return {m for r in self.requests for m in r[0]}

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def import_script(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def call_main(module, argv):
    """Run a script's main() in this process; return (exit code or message, stdout, stderr)."""
    old_argv = sys.argv
    sys.argv = [module.__name__] + argv
    out, err = io.StringIO(), io.StringIO()
    code = 0
    try:
        with redirect_stdout(out), redirect_stderr(err):
            module.main()
    except SystemExit as e:
        code = e.code
    finally:
        sys.argv = old_argv
    return code, out.getvalue(), err.getvalue()


def non_loopback_address():
    """An IPv4 address of this computer that is not a loopback address, or None. Connecting a UDP
    socket sends nothing; it only chooses the local address of the route."""
    candidates = []
    try:
        candidates += [info[4][0] for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)]
    except OSError:
        pass
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))
            candidates.append(s.getsockname()[0])
    except OSError:
        pass
    for address in candidates:
        if not ipaddress.ip_address(address).is_loopback:
            try:
                with socket.socket() as s:
                    s.bind((address, 0))
                return address
            except OSError:
                pass
    return None


class CountScriptsTest(BitcoinTestFramework):
    def set_test_params(self):
        # Node 1, with a wallet, puts MWEB data on a chain of its own; it is started only when the
        # wallet is built.
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.wallet_names = []
        auth_hash = hmac.new(RPC_SALT.encode(), RPC_PASSWORD.encode(), "sha256").hexdigest()
        self.extra_args = [["-disablewallet", "-rpcauth=%s:%s$%s" % (RPC_USER, RPC_SALT, auth_hash)], []]

    def setup_network(self):
        self.add_nodes(self.num_nodes, self.extra_args)
        self.start_node(0)

    # Running the scripts

    def script(self, name):
        return os.path.join(self.config["environment"]["SRCDIR"], "contrib", "devtools", name)

    def run_script(self, name, args, *, env=None, rc=0, stdin=b"", base_args=True):
        """Run a script with python3 -I in a new session (no terminal); check that its output holds
        no escape byte and no traceback."""
        environment = {k: v for k, v in os.environ.items() if k.lower() not in ("http_proxy", "https_proxy", "all_proxy", "no_proxy")}
        environment.update(env or {})
        if base_args and name != "mweb_history_check.py":
            args = ["--regtest", "--datadir", self.nodes[0].datadir] + args
        p = subprocess.run([sys.executable, "-I", self.script(name)] + args, input=stdin, capture_output=True,
                           env=environment, timeout=600, start_new_session=True)
        out, err = p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")
        assert "\x1b" not in out and "\x1b" not in err, (out, err)
        assert "\x07" not in out and "\x07" not in err
        assert "Traceback" not in err, err
        if rc is not None:
            assert_equal((p.returncode, err if p.returncode != rc else ""), (rc, ""))
        return out, err

    def node_url(self):
        return "http://127.0.0.1:%d/" % self.rpc_port

    def proxied(self, name, args, rc=0, on_request=None, on_reply=None, **kwargs):
        """Run a script through a recording proxy; return (stdout, stderr, proxy)."""
        proxy = RpcProxy(self.rpc_port)
        proxy.on_request, proxy.on_reply = on_request, on_reply
        try:
            out, err = self.run_script(name, ["--rpcurl", proxy.url()] + args, rc=rc, **kwargs)
        finally:
            proxy.close()
        allowed = {"getblockchaininfo", "getblockhash", "getblock"}
        if name == "timewarp_count.py":
            allowed.add("getblockheader")
        assert proxy.methods() <= allowed, proxy.methods()
        for methods, params, path in proxy.requests:
            assert_equal(path, "/")
            for m, p in zip(methods, params):
                if m == "getblock":
                    assert_equal(p[1], 2)
        return out, err, proxy

    # Building the chain

    def coinbase(self, height, tag, script, value):
        cb = CTransaction()
        cb.vin = [CTxIn(COutPoint(0, 0xffffffff), CScript(bytes(script_BIP34_coinbase_height(height)) + bytes(CScript([tag]))),
                        0xffffffff)]
        cb.vout = [CTxOut(value, script)]
        cb.rehash()
        return cb

    def new_block(self, height, prev_hash, ntime, version, tag, script, txs=()):
        block = create_block(int(prev_hash, 16), self.coinbase(height, tag, script, subsidy(height)), ntime,
                             version=version, txlist=list(txs))
        if any(not tx.wit.is_null() for tx in txs):
            add_witness_commitment(block)
        block.solve()
        return block

    def submit(self, block):
        assert_equal(self.nodes[0].submitblock(block.serialize().hex()), None)
        assert_equal(self.nodes[0].getbestblockhash(), block.hash)

    def spend(self, inputs, outputs):
        """inputs: (txid, n, kind, witness); outputs: (value, script)"""
        tx = CTransaction()
        tx.nVersion = 2
        for txid, n, kind, _ in inputs:
            tx.vin.append(CTxIn(COutPoint(txid, n), CScript([bytes(TRUE_SCRIPT)]) if kind == "p2sh" else b"", 0xffffffff))
        tx.vout = [CTxOut(v, s) for v, s in outputs]
        witnesses = [[bytes(TRUE_SCRIPT)] if kind == "p2wsh" else (witness or []) for _, _, kind, witness in inputs]
        if any(witnesses):
            for w in witnesses:
                tx.wit.vtxinwit.append(CTxInWitness())
                tx.wit.vtxinwit[-1].scriptWitness.stack = w
        tx.rehash()
        return tx

    def coinbase_input(self, height):
        return (self.coinbase_txid[height], 0, ["p2sh", "p2wsh", "bare"][height % 3], None)

    def record_output(self, height, tx, n, coinbase):
        script = bytes(tx.vout[n].scriptPubKey)
        if len(script) >= 4 and script[0] == 0x52 and script[1] == len(script) - 2 and 2 <= script[1] <= 40:
            self.v2[(tx.hash, n)] = dict(height=height, value=tx.vout[n].nValue, coinbase=coinbase,
                                         script=script.hex(), length=script[1])

    def build_chain(self):
        node = self.nodes[0]
        self.genesis = node.getblockhash(0)
        self.times = block_times(node.getblockheader(self.genesis)["time"])
        node.setmocktime(self.times[TIP] + 120)
        self.hashes = [self.genesis]
        self.coinbase_txid = {}
        self.v2 = {}              # outpoint (txid hex, n) -> output record
        self.v2_spends = {}       # outpoint -> (height, txid hex, expected structure outcome)
        self.coinbase_spends = []  # (spend height, spending txid hex, coinbase height, n)

        m = p2mr_construct([("m", CScript([OP_TRUE])), ("n", CScript([OP_1, OP_1]))])
        only = p2mr_construct([("only", CScript([OP_TRUE]))])
        pair = p2mr_construct([("a", CScript([OP_TRUE])), ("b", CScript([OP_2]))])
        annexed = p2mr_construct([("x", CScript([OP_3])), ("y", CScript([OP_TRUE]))])
        same = p2mr_construct([("p", CScript([OP_TRUE, OP_TRUE])), ("q", CScript([OP_3]))])
        lm, ln = m.leaves["m"], m.leaves["n"]
        # (scriptPubKey, witness, outcome)
        self.spend_cases = [
            (m.scriptPubKey, [], "step2_empty_witness"),
            (m.scriptPubKey, [lm.script], "step2_one_element"),
            (m.scriptPubKey, [lm.script, b"\x50"], "step3_annex_without_script"),
            (m.scriptPubKey, [lm.script, lm.control_block + b"\x00"], "step6_control_block_size"),
            (m.scriptPubKey, [lm.script, bytes([lm.control_block[0] & 0xfe]) + lm.control_block[1:]], "step7_control_block_parity"),
            (m.scriptPubKey, [lm.script, ln.control_block], "step10_root_differs"),
            (only.scriptPubKey, [only.leaves["only"].script, only.leaves["only"].control_block], "valid"),
            (pair.scriptPubKey, [b"\x01", pair.leaves["a"].script, pair.leaves["a"].control_block], "valid"),
            (annexed.scriptPubKey, [annexed.leaves["y"].script, annexed.leaves["y"].control_block, b"\x50\x01"], "valid"),
        ]
        self.unspent_scripts = [(CScript([OP_2, sha256(b"census unspent %d" % i)]), v)
                                for i, v in enumerate([250000000, 725000000, 12500000])]
        other = [(30000000 + length, CScript([OP_2, bytes(range(length))])) for length in (2, 20, 40)]
        look_alikes = [
            CScript([OP_1, sha256(b"v1")]),
            CScript([OP_3, sha256(b"v3")]),
            CScript([OP_2, b"\x07"]),
            CScript([OP_2, bytes(41)]),
            CScript([OP_HASH160, hash160(bytes(CScript([OP_2, sha256(b"wrapped")]))), OP_EQUAL]),
            CScript(bytes([0x52, 0x4c, 0x20]) + sha256(b"pushdata1")),
            CScript(bytes(CScript([OP_2, sha256(b"extra")])) + b"\x51"),
        ]
        assert_equal(bytes(look_alikes[2]).hex(), "520107")
        assert_equal(bytes(look_alikes[3])[:2].hex(), "5229")

        early_at = {c + d: c for c, d in EARLY}
        for h in range(1, TIP + 1):
            txs = []
            if h == 130:
                outputs = [(1000000 * (i + 1), s) for i, (s, _, _) in enumerate(self.spend_cases)]
                outputs += [(v, s) for s, v in self.unspent_scripts] + other + [(1000000, s) for s in look_alikes]
                change = subsidy(1) - sum(v for v, _ in outputs)
                fund = self.spend([self.coinbase_input(1)], outputs + [(change, TRUE_SCRIPT)])
                self.coinbase_spends.append((h, fund.hash, 1, 0))
                txs.append(fund)
                self.fund = fund
            if h == 140:
                for i, (_, witness, outcome) in enumerate(self.spend_cases):
                    tx = self.spend([(self.fund.sha256, i, "v2", witness)], [(self.fund.vout[i].nValue, TRUE_SCRIPT)])
                    self.v2_spends[(self.fund.hash, i)] = (h, tx.hash, outcome)
                    txs.append(tx)
                n20 = len(self.spend_cases) + len(self.unspent_scripts) + 1
                tx = self.spend([(self.fund.sha256, n20, "v2", None)], [(self.fund.vout[n20].nValue, TRUE_SCRIPT)])
                self.v2_spends[(self.fund.hash, n20)] = (h, tx.hash, None)
                txs.append(tx)
            if h == 145:
                leaf = same.leaves["p"]
                create = self.spend([self.coinbase_input(2)], [(90000000, same.scriptPubKey), (subsidy(2) - 90000000, TRUE_SCRIPT)])
                self.coinbase_spends.append((h, create.hash, 2, 0))
                spend = self.spend([(create.sha256, 0, "v2", [leaf.script, leaf.control_block])], [(90000000, TRUE_SCRIPT)])
                self.v2_spends[(create.hash, 0)] = (h, spend.hash, "valid")
                txs += [create, spend]
            if h in early_at:
                c = early_at[h]
                tx = self.spend([self.coinbase_input(c)], [(subsidy(c), TRUE_SCRIPT)])
                self.coinbase_spends.append((h, tx.hash, c, 0))
                txs.append(tx)
            script = CScript([OP_2, sha256(b"coinbase v2")]) if h == V2_COINBASE_HEIGHT else COINBASE_SCRIPTS[h % 3]
            block = self.new_block(h, self.hashes[-1], self.times[h], block_version(h), tag_of(h), script, txs)
            self.submit(block)
            self.hashes.append(block.hash)
            self.coinbase_txid[h] = block.vtx[0].sha256
            self.record_output(h, block.vtx[0], 0, True)
            for tx in txs:
                for n in range(len(tx.vout)):
                    self.record_output(h, tx, n, False)
        assert_equal(node.getblockcount(), TIP)
        info = node.getblockchaininfo()
        assert not info["initialblockdownload"]
        assert "mweb" not in info["softforks"] or not info["softforks"]["mweb"]["active"]
        # The node agrees with the block times (median time past is the node's own calculation)
        for h in range(1, TIP + 1):
            assert self.times[h] > median(self.times[max(0, h - 11):h])

    # Expected reports, computed from what the test built

    def address(self, script_hex):
        decoded = self.nodes[0].decodescript(script_hex)
        found = decoded.get("addresses", [decoded["address"]] if "address" in decoded else [])
        script = bytes.fromhex(script_hex)
        if 0x51 <= script[0] <= 0x60 and len(script) >= 4 and script[1] == len(script) - 2:
            assert_equal(found, [encode_segwit_address("rfec", script[0] - 0x50, script[2:])])
        if not found:
            return "script:" + script_hex
        assert_equal(len(found), 1)
        return found[0]

    def header_lines(self, title, info):
        lines = [title, "chain=regtest blocks=%d headers=%d bestblockhash=%s initialblockdownload=%s pruned=%s" % (
            info["blocks"], info["headers"], info["bestblockhash"], "yes" if info["initialblockdownload"] else "no",
            "yes (pruneheight %d)" % info["pruneheight"] if info["pruned"] else "no"), "softforks:"]
        for name in sorted(info["softforks"]):
            fork = info["softforks"][name]
            status = "buried" if fork["type"] == "buried" else fork[fork["type"]]["status"]
            lines.append("  %s type=%s status=%s active=%s height=%s" % (
                name, fork["type"], status, "yes" if fork["active"] else "no", fork.get("height", "-")))
        return lines

    def expected_census(self, first, last, maturity=True, lowest=0, notes=(), info=None):
        info = info or self.nodes[0].getblockchaininfo()
        scan_first = max(first, lowest)
        read_from = scan_first if not maturity else max(lowest, scan_first - 99)
        out = self.header_lines("p2mr_census.py: witness version 2 outputs and early coinbase spends", info)
        out.append("range=%d-%d %s" % (scan_first, last, "partial" if notes else "complete"))
        out += ["note: " + n for n in notes]
        if read_from < scan_first:
            out.append("note: blocks %d-%d are read only for the coinbases that blocks of the range spend" % (read_from, scan_first - 1))
        created = {k: r for k, r in self.v2.items() if scan_first <= r["height"] <= last}
        spent = {k: s for k, s in self.v2_spends.items() if k in created and s[0] <= last}
        v32 = {k: r for k, r in created.items() if r["length"] == 32}
        unspent = sorted((r for k, r in v32.items() if k not in spent), key=lambda r: (-r["value"], r["height"]))
        out += ["", "(a) unspent outputs of witness version 2 with a 32-byte program, by value: %d" % len(unspent)]
        outpoint = {id(r): k for k, r in created.items()}
        for r in unspent:
            k = outpoint[id(r)]
            out.append("  value=%s height=%d coinbase=%s outpoint=%s:%d address=%s" % (
                fec(r["value"]), r["height"], "yes" if r["coinbase"] else "no", k[0], k[1], self.address(r["script"])))
        spent32 = sorted((k for k in spent if k in v32), key=lambda k: spent[k][0])
        out += ["", "(a) spent outputs of witness version 2 with a 32-byte program: %d" % len(spent32)]
        for k in spent32:
            r = v32[k]
            out.append("  height=%d value=%s coinbase=%s outpoint=%s:%d address=%s spent_height=%d spent_txid=%s bip360=%s" % (
                r["height"], fec(r["value"]), "yes" if r["coinbase"] else "no", k[0], k[1], self.address(r["script"]),
                spent[k][0], spent[k][1], spent[k][2]))
        others = sorted(((k, r) for k, r in created.items() if r["length"] != 32), key=lambda x: (x[1]["height"], x[0]))
        out += ["", "(a) outputs of witness version 2 with another program length: %d" % len(others)]
        for k, r in others:
            out.append("  height=%d length=%d value=%s outpoint=%s:%d address=%s spent_height=%s" % (
                r["height"], r["length"], fec(r["value"]), k[0], k[1], self.address(r["script"]),
                spent[k][0] if k in spent else "unspent"))
        out += ["", "signalling: blocks per window whose version has the top bits 001 and the bit set "
                "(windows of 144 blocks; windows with none are left out)"]
        maxima = [0, 0, 0]
        for w in range(scan_first // WINDOW, last // WINDOW + 1):
            counts = [sum(1 for h in range(max(scan_first, w * WINDOW), min(last, w * WINDOW + WINDOW - 1) + 1)
                          if block_version(h) & 0xe0000000 == 0x20000000 and block_version(h) >> bit & 1) for bit in (6, 7, 8)]
            if any(counts):
                maxima = [max(a, b) for a, b in zip(maxima, counts)]
                out.append("  window=%d-%d bit6=%d bit7=%d bit8=%d" % (w * WINDOW, w * WINDOW + WINDOW - 1, *counts))
        early = [(h, txid, c) for h, txid, c, _ in self.coinbase_spends
                 if scan_first <= h <= last and 10 <= h - c <= 99 and c >= read_from]
        miners = {}
        if maturity:
            out += ["", "(b) spends of coinbase outputs 10 to 99 blocks after the block that created them: %d" % len(early),
                    "by miner tag of the coinbase:"]
            for h, _, c in early:
                miners[shown_tag(tag_of(c))] = miners.get(shown_tag(tag_of(c)), 0) + 1
            out += ["  count=%d miner=%r" % (n, t) for t, n in sorted(miners.items(), key=lambda x: (-x[1], x[0]))]
            out.append("by address of the coinbase output:")
            addresses = {}
            for h, _, c in early:
                a = self.address(bytes(COINBASE_SCRIPTS[c % 3]).hex())
                addresses[a] = addresses.get(a, 0) + 1
            out += ["  count=%d address=%s" % (n, a) for a, n in sorted(addresses.items(), key=lambda x: (-x[1], x[0]))]
            out.append("by window of the spending block:")
            windows = {}
            for h, _, _ in early:
                windows[h // WINDOW] = windows.get(h // WINDOW, 0) + 1
            out += ["  window=%d-%d count=%d" % (w * WINDOW, w * WINDOW + WINDOW - 1, n) for w, n in sorted(windows.items())]
            out.append("every spend:")
            for h, txid, c in early:
                out.append("  spend_height=%d depth=%d txid=%s coinbase=%064x:0 created=%d value=%s address=%s miner=%r" % (
                    h, h - c, txid, self.coinbase_txid[c], c, fec(subsidy(c)),
                    self.address(bytes(COINBASE_SCRIPTS[c % 3]).hex()), shown_tag(tag_of(c))))
        value = sum(r["value"] for r in unspent)
        out += ["",
                "RESULT: v2_32_unspent=%d value=%s" % (len(unspent), fec(value)),
                "RESULT: v2_32_created=%d spent=%d bip360_valid=%d bip360_invalid=%d" % (
                    len(v32), len(spent32), sum(spent[k][2] == "valid" for k in spent32), sum(spent[k][2] != "valid" for k in spent32)),
                "RESULT: v2_other_created=%d" % len(others),
                "RESULT: signalling bit6_max=%d bit7_max=%d bit8_max=%d" % tuple(maxima)]
        if maturity:
            out.append("RESULT: early_coinbase_spends=%d miners=%d" % (len(early), len(miners)))
        out.append("RESULT: range=%d-%d %s" % (scan_first, last, "partial" if notes else "complete"))
        return out

    def lead(self, h):
        return self.times[h] - median(self.times[h + 1:h + 12])

    def expected_count(self, first, last, notes=(), remarks=(), lowest=0, info=None, unknown=()):
        info = info or self.nodes[0].getblockchaininfo()
        t = self.times

        def tag(h):
            return "?" if h < lowest or h in unknown else shown_tag(tag_of(h))

        out = self.header_lines("timewarp_count.py: blocks earlier than 480 s after the block 23 below them, and leads", info)
        out.append("range=%d-%d %s" % (first, last, "partial" if notes else "complete"))
        out += ["note: " + n for n in list(notes) + list(remarks)]
        spans = {h: t[h] - t[h - 23] for h in range(first, last + 1)}
        violations = [h for h in spans if spans[h] < 480]
        out += ["", "blocks whose time is earlier than 480 s after the time of the block 23 below: %d" % len(violations)]
        for h in violations:
            out.append("  height=%d hash=%s span=%d lead_23_below=%d miner=%r miner_23_below=%r" % (
                h, self.hashes[h], spans[h], self.lead(h - 23), tag(h), tag(h - 23)))
        out.append("by span: below_240=%d 240_to_399=%d 400_to_479=%d" % (
            sum(spans[h] < 240 for h in violations), sum(240 <= spans[h] < 400 for h in violations),
            sum(400 <= spans[h] for h in violations)))
        out.append("by miner:")
        counts = {}
        for h in violations:
            counts[tag(h)] = counts.get(tag(h), 0) + 1
        out += ["  count=%d miner=%r" % (n, m) for m, n in sorted(counts.items(), key=lambda x: (-x[1], x[0]))]
        leads = {h: self.lead(h) for h in range(first, last - 10)}
        if leads:
            out += ["", "leads (time minus the median time of the 11 blocks after it) of blocks %d-%d:" % (first, last - 11)]
        else:
            out += ["", "leads: the range has no block with 11 blocks after it in the range"]
        over = []
        for limit in (600, 1200, 1800, 3600):
            blocks = [h for h in leads if leads[h] > limit]
            over.append(len(blocks))
            out.append("lead over %d s: %d" % (limit, len(blocks)))
            counts = {}
            for h in blocks:
                counts[tag(h)] = counts.get(tag(h), 0) + 1
            out += ["  count=%d miner=%r" % (n, m) for m, n in sorted(counts.items(), key=lambda x: (-x[1], x[0]))]
        largest = None
        for h in sorted(leads):
            if largest is None or leads[h] > leads[largest]:
                largest = h
        if largest is not None:
            out.append("largest lead: lead=%d height=%d hash=%s miner=%r" % (leads[largest], largest, self.hashes[largest], tag(largest)))
        out += ["",
                "RESULT: floor_violations=%d per_100000=%.2f smallest_span=%d" % (
                    len(violations), len(violations) * 100000 / (last - first + 1), min(spans.values())),
                "RESULT: ahead_over_600=%d ahead_over_1200=%d ahead_over_1800=%d ahead_over_3600=%d largest_lead=%s" % (
                    tuple(over) + ("none" if largest is None else str(leads[largest]),)),
                "RESULT: mean_interval=%d" % ((t[last] - t[first]) // (last - first) if last > first else 0),
                "RESULT: range=%d-%d %s" % (first, last, "partial" if notes else "complete")]
        return out

    def check_report(self, out, expected):
        assert_equal(out.splitlines(), expected)

    # The test

    def run_test(self):
        node = self.nodes[0]
        self.rpc_port = rpc_port(0)
        self.log.info("Build a chain of %d blocks" % TIP)
        self.build_chain()
        # A transaction in the mempool, which must still be there afterwards
        mempool_tx = self.spend([self.coinbase_input(3)], [(subsidy(3) - COIN // 1000, CScript([OP_0, bytes(20)]))])
        node.sendrawtransaction(mempool_tx.serialize().hex())
        state = (node.getbestblockhash(), node.gettxoutsetinfo()["hash_serialized_2"], node.getrawmempool())

        self.test_fixed_values()
        self.test_census()
        self.test_count()
        self.test_measure_and_common_part()
        self.test_allowlist()
        self.test_authentication()
        self.test_connection_safety()
        self.test_output_safety()
        self.test_limits_and_retries()
        self.test_partial_nodes()
        assert_equal((node.getbestblockhash(), node.gettxoutsetinfo()["hash_serialized_2"], node.getrawmempool()), state)
        self.test_reorganisations()
        self.test_interrupted()
        self.test_mweb_history_check()
        assert_equal((node.getbestblockhash(), node.gettxoutsetinfo()["hash_serialized_2"], node.getrawmempool()), state)
        self.test_headers_ahead()
        self.test_mweb_node()

    def test_fixed_values(self):
        self.log.info("Check the chain's figures against fixed values")
        census = self.expected_census(0, TIP)
        assert_equal([l for l in census if l.startswith("RESULT: ")], [
            "RESULT: v2_32_unspent=4 value=59.87500000",
            "RESULT: v2_32_created=14 spent=10 bip360_valid=4 bip360_invalid=6",
            "RESULT: v2_other_created=3",
            "RESULT: signalling bit6_max=108 bit7_max=107 bit8_max=5",
            "RESULT: early_coinbase_spends=4 miners=3",
            "RESULT: range=0-760 complete",
        ])
        count = self.expected_count(24, TIP)
        assert_equal([l for l in count if l.startswith("RESULT: ")], [
            "RESULT: floor_violations=%d per_100000=%.2f smallest_span=%d" % (
                len([h for h in range(24, TIP + 1) if self.times[h] - self.times[h - 23] < 480]),
                len([h for h in range(24, TIP + 1) if self.times[h] - self.times[h - 23] < 480]) * 100000 / (TIP - 23),
                1380 - 5000),
            "RESULT: ahead_over_600=11 ahead_over_1200=8 ahead_over_1800=6 ahead_over_3600=2 largest_lead=4640",
            "RESULT: mean_interval=%d" % ((self.times[TIP] - self.times[24]) // (TIP - 24)),
            "RESULT: range=24-760 complete",
        ])
        spans = {h: self.times[h] - self.times[h - 23] for h in range(24, TIP + 1)}
        assert_equal((spans[24], spans[25]), (480, 479))
        assert_equal([spans[h] for h in (123, 223)], [460, 380])
        assert_equal(sorted(self.lead(h) for h in AHEAD if h < TIP - 10),
                     [600, 601, 640, 1200, 1201, 1800, 1801, 2140, 3440, 3600, 3601, 4640])
        assert_equal(self.lead(200), 640)
        assert self.times[23] - self.times[0] < 480
        assert_equal(len([h for h in range(24, TIP + 1) if self.times[h] - self.times[h - 23] < 480]), 27)

    def test_census(self):
        self.log.info("p2mr_census.py: the whole chain, directly and through a recording proxy")
        expected = self.expected_census(0, TIP)
        out, err = self.run_script("p2mr_census.py", ["--rpcurl", self.node_url()])
        self.check_report(out, expected)
        out, _, proxy = self.proxied("p2mr_census.py", [])
        self.check_report(out, expected)
        assert_equal(proxy.methods(), {"getblockchaininfo", "getblockhash", "getblock"})
        out, _, _ = self.proxied("p2mr_census.py", ["--batch", "7"])
        self.check_report(out, expected)

        self.log.info("p2mr_census.py: partial ranges count their first and last heights")
        out, _, _ = self.proxied("p2mr_census.py", ["--start", "130", "--end", "140"])
        self.check_report(out, self.expected_census(130, 140, notes=["the range does not cover the whole chain (0-760)"]))
        out, _, _ = self.proxied("p2mr_census.py", ["--start", "131", "--end", "145"])
        self.check_report(out, self.expected_census(131, 145, notes=["the range does not cover the whole chain (0-760)"]))
        self.log.info("p2mr_census.py: coinbases created before the range are read for the early spends")
        for start in (305, 400, 403):
            out, _, _ = self.proxied("p2mr_census.py", ["--start", str(start)])
            self.check_report(out, self.expected_census(start, TIP, notes=["the range does not cover the whole chain (0-760)"]))
        out, _, _ = self.proxied("p2mr_census.py", ["--start", "310", "--end", "352"])
        self.check_report(out, self.expected_census(310, 352, notes=["the range does not cover the whole chain (0-760)"]))
        out, _, _ = self.proxied("p2mr_census.py", ["--no-maturity"])
        self.check_report(out, self.expected_census(0, TIP, maturity=False))
        out, _, _ = self.proxied("p2mr_census.py", ["--start", "200", "--end", "300", "--no-maturity"])
        self.check_report(out, self.expected_census(200, 300, maturity=False, notes=["the range does not cover the whole chain (0-760)"]))
        _, err, _ = self.proxied("p2mr_census.py", ["--start", "10", "--end", "9"], rc=1)
        assert_equal(err, "--start and --end must give 0 <= start <= end <= 760 (the tip)\n")

    def test_count(self):
        self.log.info("timewarp_count.py: the default range covers the chain from height 24")
        expected = self.expected_count(24, TIP)
        out, _ = self.run_script("timewarp_count.py", ["--rpcurl", self.node_url()])
        self.check_report(out, expected)
        out, _, proxy = self.proxied("timewarp_count.py", [])
        self.check_report(out, expected)
        assert_equal(proxy.methods(), {"getblockchaininfo", "getblockhash", "getblockheader", "getblock"})
        listed = [l for l in out.splitlines() if l.startswith("  height=")]
        assert "  height=25 hash=%s span=479 lead_23_below=%d miner=%r miner_23_below=%r" % (
            self.hashes[25], self.lead(2), "/pool-b/", "/pool-c/") in listed
        assert not [l for l in listed if l.startswith("  height=24 ")]
        assert "  height=223 hash=%s span=380 lead_23_below=640 miner=%r miner_23_below=%r" % (
            self.hashes[223], "/pool-b/", "/pool-c/") in listed
        assert "  height=123 hash=%s span=460 lead_23_below=-120 miner=%r miner_23_below=%r" % (
            self.hashes[123], HOSTILE_SHOWN, "/pool-b/") in listed
        out, _, _ = self.proxied("timewarp_count.py", ["--batch", "3"])
        self.check_report(out, expected)

        self.log.info("timewarp_count.py: heights below 24 are left out")
        out, _, _ = self.proxied("timewarp_count.py", ["--start", "0"])
        self.check_report(out, self.expected_count(24, TIP, remarks=["the rule applies from height 24, so the range starts there"]))
        _, err, _ = self.proxied("timewarp_count.py", ["--start", "0", "--end", "23"], rc=1)
        assert_equal(err, "the range has no block at height 24 or above\n")

        self.log.info("timewarp_count.py: partial ranges count their first and last heights")
        partial = ["the range does not cover the chain from height 24 (24-760)"]
        out, _, _ = self.proxied("timewarp_count.py", ["--start", "123", "--end", "223"])
        self.check_report(out, self.expected_count(123, 223, partial))
        assert "floor_violations=2 " in out
        self.log.info("timewarp_count.py: the last 11 blocks of the range have no lead")
        out, _, _ = self.proxied("timewarp_count.py", ["--start", "450", "--end", "510"])
        self.check_report(out, self.expected_count(450, 510, partial))
        assert "largest_lead=4640" not in out
        out, _, _ = self.proxied("timewarp_count.py", ["--start", "450", "--end", "511"])
        self.check_report(out, self.expected_count(450, 511, partial))
        assert "largest_lead=4640" in out
        out, _, _ = self.proxied("timewarp_count.py", ["--start", "700", "--end", "705"])
        self.check_report(out, self.expected_count(700, 705, partial))
        assert "largest_lead=none" in out

    def test_measure_and_common_part(self):
        self.log.info("measure() with values no chain has, and the common part of the two scripts")
        count = import_script(self.script("timewarp_count.py"), "timewarp_count")
        # Each median differs from its neighbours, so an index one off gives other leads.
        leads = [600, 601, 1200, 1201, 1800, 1801, 3600, 3601]
        medians = [50000 - 13 * i for i in range(len(leads) + 11)]
        times = [medians[i + 11] + lead for i, lead in enumerate(leads)] + [0] * 11
        assert_equal(count.measure(times, medians), leads)
        assert_equal(count.measure([5] * 11, [0] * 11), [])
        assert_equal(count.measure([7] * 12, list(range(12))), [7 - 11])
        sources = []
        for name in ("p2mr_census.py", "timewarp_count.py"):
            with open(self.script(name), encoding="utf8") as f:
                text = f.read()
            start = text.index("# --- Common part")
            sources.append(text[start:text.index("# --- End of the common part ---", start)])
            assert "class NodeRpc" in sources[-1] and "urllib.request" not in text and "requests" not in text.split("ALLOWED")[0]
        assert_equal(sources[0], sources[1])

        self.log.info("The constants of each network, the bounds of an amount and the length of a miner tag")
        for name in ("p2mr_census.py", "timewarp_count.py"):
            module = import_script(self.script(name), name[:-3])
            assert_equal(module.CHAINS, CHAIN_CONSTANTS)
            assert_equal(module.checked_amount(decimal.Decimal("60221400")), 60221400 * COIN)
            for value in ("1E+999999", "-1E+999999", "60221400.00000001", "1E-9", "-1"):
                try:
                    module.checked_amount(decimal.Decimal(value))
                except SystemExit as e:
                    assert_equal(e.code, NOT_A_NODE)
                else:
                    raise AssertionError("amount %s accepted" % value)
            tag = "".join(chr(ord("A") + i % 26) for i in range(100))
            assert_equal(module.miner_tag((bytes([3, 1, 2, 3]) + tag.encode()).hex()), tag[:60])

    def test_allowlist(self):
        self.log.info("The RPC class refuses methods and parameters it does not use before sending")
        proxy = RpcProxy(self.rpc_port)
        for name, bad in (("p2mr_census.py", ["getblockheader", [self.genesis, True]]), ("timewarp_count.py", None)):
            assert_equal(sorted(import_script(self.script(name), name[:-3]).ALLOWED_METHODS),
                         ["getblock", "getblockchaininfo", "getblockhash"] + (["getblockheader"] if bad is None else []))
            module = import_script(self.script(name), name[:-3])
            rpc = module.NodeRpc("127.0.0.1", proxy.port, "u", "p", 0)
            calls = [("stop", []), ("sendrawtransaction", ["00"]), ("getblock", [self.genesis, 1]), ("getblock", [self.genesis.upper(), 2]),
                     ("getblockhash", ["1"]), ("getblockhash", [True]), ("getblockhash", [-1]), ("getblockchaininfo", [1]),
                     ("getblockheader", [self.genesis, False]), ("getblock", [self.genesis])]
            if bad:
                calls.append(tuple(bad))
            for method, params in calls:
                try:
                    with redirect_stderr(io.StringIO()):
                        rpc.batch([("getblockchaininfo", []), (method, params)])
                    raise AssertionError("sent %s %r" % (method, params))
                except SystemExit as e:
                    if method in module.ALLOWED_METHODS:
                        assert_equal(e.code, "refused to send %s with parameters this script does not use" % method)
                    else:
                        assert_equal(e.code, "refused to send %r: this script sends only %s" % (method, ", ".join(sorted(module.ALLOWED_METHODS))))
        assert_equal(proxy.requests, [])
        proxy.close()

    def test_authentication(self):
        node = self.nodes[0]
        self.log.info("Authentication: the cookie at the default place of the data directory")
        expected = self.expected_count(24, TIP)
        _, _, proxy = self.proxied("timewarp_count.py", [])
        with open(os.path.join(node.datadir, "regtest", ".cookie"), encoding="ascii") as f:
            cookie_auth = "Basic " + base64.b64encode(f.read().strip().encode()).decode()
        assert_equal(set(proxy.auth), {cookie_auth})

        self.log.info("Authentication: --rpcuser and --rpcpassword")
        password_auth = "Basic " + base64.b64encode(("%s:%s" % (RPC_USER, RPC_PASSWORD)).encode()).decode()
        out, _, proxy = self.proxied("timewarp_count.py", ["--rpcuser", RPC_USER, "--rpcpassword", RPC_PASSWORD])
        self.check_report(out, expected)
        assert_equal(set(proxy.auth), {password_auth})
        out, err = self.run_script("timewarp_count.py", ["--rpcuser", RPC_USER, "--rpcpassword", "wrong", "--rpcurl", self.node_url()], rc=1)
        assert_equal(out, "")
        assert err.endswith("RPC authentication failed (HTTP 401): check --datadir or --rpcuser/--rpcpassword.\n")

        self.log.info("Authentication: a refusal after the node accepted the cookie suggests that the node restarted")
        def refuse_after_first(n, calls):
            return (401, b"") if n >= 1 else None
        for name in ("p2mr_census.py", "timewarp_count.py"):
            out, err, _ = self.proxied(name, [], rc=1, on_request=refuse_after_first)
            assert_equal((out, err), ("", AUTH_FAILED_LATER + "\n"))
        out, err, _ = self.proxied("timewarp_count.py", ["--rpcuser", RPC_USER, "--rpcpassword", RPC_PASSWORD], rc=1,
                                   on_request=refuse_after_first)
        assert_equal((out, err), ("", AUTH_FAILED + "\n"))
        out, err, _ = self.proxied("timewarp_count.py", [], rc=1, on_request=lambda n, calls: (401, b""))
        assert_equal((out, err), ("", AUTH_FAILED + "\n"))

        self.log.info("Authentication: rpcuser and rpcpassword in ferrite.conf, no cookie")
        for i, conf in enumerate(["rpcuser=%s\nrpcpassword=%s\n" % (RPC_USER, RPC_PASSWORD),
                                  "# comment\nrpcuser=wrong\nrpcpassword=wrong\n[main]\nrpcuser=main\n[regtest]\n"
                                  "rpcuser = %s  # the user\nrpcpassword=%s\nrpcpassword=later\n" % (RPC_USER, RPC_PASSWORD),
                                  "regtest.rpcuser=%s\nregtest.rpcpassword=%s\n[test]\nrpcuser=x\n" % (RPC_USER, RPC_PASSWORD)]):
            datadir = os.path.join(self.options.tmpdir, "conf%d" % i)
            os.makedirs(os.path.join(datadir, "regtest"))
            with open(os.path.join(datadir, "ferrite.conf"), "w", encoding="utf8") as f:
                f.write(conf)
            proxy = RpcProxy(self.rpc_port)
            out, _ = self.run_script("timewarp_count.py", ["--regtest", "--datadir", datadir, "--rpcurl", proxy.url()], base_args=False)
            proxy.close()
            self.check_report(out, expected)
            assert_equal(set(proxy.auth), {password_auth})
        self.log.info("Authentication: with neither, the script asks, and names the directory")
        empty = os.path.join(self.options.tmpdir, "empty")
        os.makedirs(empty)
        proxy = RpcProxy(self.rpc_port)
        out, err = self.run_script("timewarp_count.py", ["--regtest", "--datadir", empty, "--rpcurl", proxy.url()], base_args=False,
                                   stdin=("%s\n%s\n" % (RPC_USER, RPC_PASSWORD)).encode())
        proxy.close()
        self.check_report(out, expected)
        assert ("no .cookie in %s and no rpcuser in ferrite.conf" % os.path.join(empty, "regtest")) in err
        assert RPC_PASSWORD not in err
        assert_equal(set(proxy.auth), {password_auth})

        self.log.info("Authentication: a host that is not this computer's loopback gets no local credentials")
        address = non_loopback_address()
        if address is None:
            self.log.info("  (no address other than loopback; the end-to-end case is skipped)")
        else:
            proxy = RpcProxy(self.rpc_port, host=address)
            _, err = self.run_script("timewarp_count.py", ["--rpcurl", proxy.url()], stdin=b"someone\nguess\n", rc=1)
            assert err.endswith("RPC authentication failed (HTTP 401): check --datadir or --rpcuser/--rpcpassword.\n")
            assert "no .cookie" not in err
            out, _ = self.run_script("p2mr_census.py", ["--rpcurl", proxy.url(), "--rpcuser", RPC_USER, "--rpcpassword", RPC_PASSWORD,
                                                        "--start", "700"])
            proxy.close()
            assert_equal(proxy.auth[0], "Basic " + base64.b64encode(b"someone:guess").decode())
            assert cookie_auth not in proxy.auth
            assert password_auth in proxy.auth
        census = import_script(self.script("p2mr_census.py"), "p2mr_census")
        for host in ("192.0.2.1", "example.com", "::ffff:192.0.2.1", "127.example.com", "::", "0.0.0.0"):
            assert not census.is_loopback(host)
        for host in ("127.0.0.1", "127.1.2.3", "::1", "localhost", "::ffff:127.0.0.1"):
            assert census.is_loopback(host)

        self.log.info("Authentication: the cookie and the ferrite.conf section of each network")
        networks = os.path.join(self.options.tmpdir, "networks")
        cases = [([], 9573, "main", ""), (["--testnet"], 19573, "test", "testnet4"), ([], 19573, "test", "testnet4"),
                 (["--regtest"], 19443, "regtest", "regtest")]
        for _, _, name, subdir in cases:
            os.makedirs(os.path.join(networks, subdir), exist_ok=True)
            with open(os.path.join(networks, subdir, ".cookie"), "w", encoding="ascii") as f:
                f.write("__cookie__:%s-cookie" % name)
        for flags, port, name, _ in cases:
            args = census.argparse.Namespace(rpcuser=None, rpcpassword=None, datadir=networks, testnet="--testnet" in flags,
                                             regtest="--regtest" in flags)
            assert_equal(census.credentials(args, "127.0.0.1", port), ("__cookie__", "%s-cookie" % name))
            assert_equal(census.credentials(args, "localhost", port), ("__cookie__", "%s-cookie" % name))
        for _, _, _, subdir in cases:
            if os.path.exists(os.path.join(networks, subdir, ".cookie")):
                os.remove(os.path.join(networks, subdir, ".cookie"))
        with open(os.path.join(networks, "ferrite.conf"), "w", encoding="utf8") as f:
            f.write("rpcpassword=shared\n[main]\nrpcuser=main-user\n[test]\nrpcuser=test-user\nrpcpassword=test-password\n"
                    "[regtest]\nrpcuser=regtest-user\n")
        expected = {"main": ("main-user", "shared"), "test": ("test-user", "test-password"), "regtest": ("regtest-user", "shared")}
        for flags, port, name, _ in cases:
            args = census.argparse.Namespace(rpcuser=None, rpcpassword=None, datadir=networks, testnet="--testnet" in flags,
                                             regtest="--regtest" in flags)
            assert_equal(census.credentials(args, "127.0.0.1", port), expected[name])
        platform, appdata = sys.platform, os.environ.get("APPDATA")
        try:
            sys.platform, os.environ["APPDATA"] = "win32", os.path.join("C:", "Users", "x", "AppData", "Roaming")
            assert_equal(census.default_datadir(), os.path.join("C:", "Users", "x", "AppData", "Roaming", "Ferrite"))
            sys.platform = "darwin"
            assert census.default_datadir().endswith(os.path.join("Library", "Application Support", "Ferrite"))
        finally:
            sys.platform = platform
            if appdata is None:
                del os.environ["APPDATA"]
            else:
                os.environ["APPDATA"] = appdata
        assert census.default_datadir().endswith(".ferrite")

        self.log.info("Authentication: a mistyped or shortened password option is not repeated")
        for name in ("p2mr_census.py", "timewarp_count.py"):
            for args in (["--rpcpasswrd", "SECRET"], ["--rpcpass", "SECRET"], ["--rpcpassword=SECRET", "--start", "x"],
                         ["--rpcuser", "SECRET", "--retries", "SECRET"], ["SECRET"]):
                out, err = self.run_script(name, args, rc=2)
                assert "SECRET" not in out + err, err
                assert err.endswith("invalid or unrecognised arguments; see --help\n"), err
            _, err = self.run_script(name, ["--rpcpassword", "SECRET", "--rpcurl", self.node_url()], rc=1)
            assert_equal(err, "--rpcpassword needs --rpcuser\n")

    def check_refused_urls(self, name, listener):
        p = listener.port
        extra = ["--rpcuser", "u", "--rpcpassword", "p"] if name == "mweb_history_check.py" else []
        cases = [("ftp://127.0.0.1:%d/" % p, BAD_URL), ("file:///SECRETURL", BAD_URL), ("https://127.0.0.1:%d/" % p, NO_TLS),
                 ("HTTPS://SECRETURL:%d/" % p, NO_TLS), ("127.0.0.1:%d" % p, BAD_URL), ("//127.0.0.1:%d/" % p, BAD_URL),
                 ("http://127.0.0.1/", BAD_URL), ("http://127.0.0.1:0/", BAD_URL), ("http://127.0.0.1:65536/", BAD_URL),
                 ("http://127.0.0.1:SECRETURL/", BAD_URL), ("http://:%d/" % p, BAD_URL),
                 ("http://user:SECRETURL@127.0.0.1:%d/" % p, BAD_URL), ("http://SECRETURL@127.0.0.1:%d/" % p, BAD_URL),
                 ("http://127.0.0.1:%d/wallet/SECRETURL" % p, BAD_URL), ("http://127.0.0.1:%d/?SECRETURL" % p, BAD_URL),
                 ("http://127.0.0.1:%d/#SECRETURL" % p, BAD_URL), ("http://127.0.0.1:%d/?" % p, BAD_URL),
                 ("http://a..b:%d/" % p, BAD_URL), ("http://.a:%d/" % p, BAD_URL), ("http://%s:%d/" % ("a" * 64, p), BAD_URL)]
        if not extra:
            # Python's URL parser drops tabs and newlines; these scripts refuse such URLs before parsing them.
            cases += [("http://127.0.0.1:%d/\n" % p, BAD_URL), ("http://127.0.0.1:%d\t/" % p, BAD_URL),
                      ("http://127.0.0.1:%d/ " % p, BAD_URL), ("http://127.0.0.1%%2e:%d/" % p, BAD_URL)]
        for url, message in cases:
            out, err = self.run_script(name, extra + ["--rpcurl", url], rc=1)
            assert_equal((out, err), ("", message + "\n"))
        assert_equal(listener.accepted, 0)

    def test_connection_safety(self):
        self.log.info("Connections: proxy variables of the environment are not used")
        listener = Listener()
        env = {v: "http://127.0.0.1:%d/" % listener.port for v in PROXY_VARIABLES}
        out, _ = self.run_script("p2mr_census.py", ["--rpcurl", self.node_url()], env=env)
        self.check_report(out, self.expected_census(0, TIP))
        out, _ = self.run_script("timewarp_count.py", ["--rpcurl", self.node_url()], env=env)
        self.check_report(out, self.expected_count(24, TIP))
        out, _ = self.run_script("timewarp_count.py", ["--rpcurl", "http://localhost:%d/" % self.rpc_port], env=env)
        self.check_report(out, self.expected_count(24, TIP))
        assert_equal(listener.accepted, 0)

        self.log.info("Connections: redirects are reported, not followed")
        for name in ("p2mr_census.py", "timewarp_count.py"):
            for status in (301, 302, 303, 307, 308):
                proxy = RpcProxy(self.rpc_port, redirect=(status, "http://127.0.0.1:%d/" % listener.port))
                _, err = self.run_script(name, ["--rpcurl", proxy.url()], rc=1)
                proxy.close()
                assert_equal(err, "127.0.0.1:%d answered with HTTP %d, a redirect, which is not followed\n" % (proxy.port, status))
                assert_equal(len(proxy.requests), 1)
        assert_equal(listener.accepted, 0)

        self.log.info("Connections: URLs other than http://HOST:PORT/ are refused before any request")
        for name in ("p2mr_census.py", "timewarp_count.py"):
            self.check_refused_urls(name, listener)
        listener.close()

    def test_output_safety(self):
        self.log.info("Output: a raw server that answers with escape sequences")
        answers = [b"HTTP/1.1 200 OK\r\nContent-Length: 24\r\n\r\n\x1b]52;c;SGVsbG8=\x07\x1b[2J[]\n",
                   b"HTTP/1.1 200 \x1b[31mOK\r\nContent-Length: 2\r\n\r\n[]",
                   b"\x1b]52;c;SGVsbG8=\x07 200 OK\r\n\r\n",
                   b"HTTP/1.1 404 \x1b]52;c;SGVsbG8=\x07\nRESULT: v2_32_unspent=0\r\nContent-Length: 0\r\n\r\n",
                   b"HTTP/1.1 200 OK\r\nX-\x1b[2J: \x1b[2J\r\nContent-Length: 30\r\n\r\n" + json.dumps([{"id": 0, "result": None, "error": None}]).encode()[:30]]
        for name in ("p2mr_census.py", "timewarp_count.py"):
            for answer in answers:
                server = RawServer(answer)
                out, err = self.run_script(name, ["--rpcurl", "http://127.0.0.1:%d/" % server.port, "--retries", "0"], rc=1)
                server.close()
                assert_equal(out, "")
                assert err in (NOT_A_NODE + "\n", "127.0.0.1:%d answered with HTTP 404\n" % server.port), err

        self.log.info("Output: well-formed replies whose strings hold escape sequences and result lines")
        hostile_fork = {"type": "bip9", "bip9": {"status": "active"}, "active": True, "height": 0}

        def change(path, value):
            def on_reply(n, calls, replies):
                for call, reply in zip(calls, replies):
                    if call["method"] == path[0] and reply.get("result") is not None:
                        target = reply["result"]
                        for key in path[1:-1]:
                            target = target[key]
                        target[path[-1]] = value(target.get(path[-1])) if callable(value) else value
                return replies
            return on_reply

        both, census = ("p2mr_census.py", "timewarp_count.py"), ("p2mr_census.py",)
        cases = [
            (change(("getblockchaininfo", "chain"), "regtest\x1b[31m"), both),
            (change(("getblockchaininfo", "chain"), "regtest\nRESULT: v2_32_unspent=0"), both),
            (change(("getblockchaininfo", "chain"), "signet"), both),
            (change(("getblockchaininfo", "chain"), ["regtest"]), both),
            (change(("getblockchaininfo", "chain"), {"regtest": 1}), both),
            (change(("getblockchaininfo", "softforks", HOSTILE_TEXT), hostile_fork), both),
            (change(("getblockchaininfo", "softforks", "taproot", "bip9", "status"), "active" + HOSTILE_TEXT), both),
            (change(("getblockchaininfo", "bestblockhash"), lambda v: v[:-1] + "\x1b"), both),
            (change(("getblockchaininfo", "blocks"), "760"), both),
            (change(("getblockchaininfo", "headers"), 7.6e2), both),
            (change(("getblock", "tx", 0, "txid"), lambda v: v.upper()), census),
            (change(("getblock", "tx", 0, "vout", 0, "scriptPubKey", "hex"), lambda v: v + "\x1b"), census),
            (change(("getblock", "tx", 0, "vout", 0, "value"), "50"), census),
            (change(("getblock", "tx", 0, "vout", 0, "value"), -1), census),
            (change(("getblock", "tx", 0, "vin", 0, "ismweb"), 0), census),
            (change(("getblock", "tx", 0, "vin", 0, "coinbase"), lambda v: v + "\x1b"), both),
            (change(("getblock", "height"), "1"), both),
            (change(("getblock", "height"), lambda v: v + 1), both),
            (change(("getblockheader", "height"), lambda v: v + 1), ("timewarp_count.py",)),
            (change(("getblock", "hash"), "0" * 64), both),
            (change(("getblockheader", "time"), -1), ("timewarp_count.py",)),
            (change(("getblockheader", "mediantime"), "1"), ("timewarp_count.py",)),
        ]
        for on_reply, names in cases:
            for name in names:
                out, err, _ = self.proxied(name, ["--end", "200"] if name == "p2mr_census.py" else [], rc=1, on_reply=on_reply)
                assert_equal((out, err), ("", NOT_A_NODE + "\n"))

        self.log.info("Output: a node error message is printed filtered")
        def error_reply(n, calls, replies):
            if calls[0]["method"] == "getblockhash":
                replies[3] = {"id": replies[3]["id"], "result": None, "error": {"code": -1, "message": HOSTILE_TEXT + "\x1b[2J"}}
            return replies
        for name in ("p2mr_census.py", "timewarp_count.py"):
            out, err, _ = self.proxied(name, [], rc=1, on_reply=error_reply)
            assert_equal((out, err), ("", "the node answered getblockhash with error -1: %r\n" % "]52;c;SGVsbG8=RESULT: v2_32_unspent=0[2J"))

        self.log.info("Output: address fields of a reply are not used")
        def address_reply(n, calls, replies):
            for reply in replies:
                result = reply.get("result")
                if isinstance(result, dict) and "tx" in result:
                    for tx in result["tx"]:
                        for txout in tx["vout"]:
                            txout["scriptPubKey"]["address"] = HOSTILE_TEXT
                            txout["scriptPubKey"]["addresses"] = [HOSTILE_TEXT]
            return replies
        out, _, _ = self.proxied("p2mr_census.py", [], on_reply=address_reply)
        self.check_report(out, self.expected_census(0, TIP))
        out, _, _ = self.proxied("timewarp_count.py", [], on_reply=address_reply)
        self.check_report(out, self.expected_count(24, TIP))
        lines = out.splitlines()
        assert_equal([l for l in lines if l.startswith("RESULT: ")], lines[-4:])

    def test_limits_and_retries(self):
        self.log.info("Limits: a reply that announces more than 256 MiB is refused unread")
        big = RawServer(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n[" % (256 * 1024 * 1024 + 1))
        for name in ("p2mr_census.py", "timewarp_count.py"):
            out, err = self.run_script(name, ["--rpcurl", "http://127.0.0.1:%d/" % big.port], rc=1)
            assert_equal((out, err), ("", "a reply was larger than 256 MiB; run again with a smaller --batch\n"))
        big.close()
        self.log.info("Limits: a longer reply without its length is cut at the limit (the limit made small here)")
        for name in ("p2mr_census.py", "timewarp_count.py"):
            module = import_script(self.script(name), name[:-3])
            assert_equal(module.MAX_REPLY_BYTES, 256 * 1024 * 1024)
            module.MAX_REPLY_BYTES = 3000
            code, out, _ = call_main(module, ["--regtest", "--datadir", self.nodes[0].datadir, "--rpcurl", self.node_url()])
            assert_equal((code, out), ("a reply was larger than 256 MiB; run again with a smaller --batch", ""))
            unannounced = RawServer(b"HTTP/1.0 200 OK\r\n\r\n[" + b" " * 5000 + b"]")
            code, out, _ = call_main(module, ["--regtest", "--datadir", self.nodes[0].datadir, "--rpcurl",
                                              "http://127.0.0.1:%d/" % unannounced.port])
            unannounced.close()
            assert_equal((code, out), ("a reply was larger than 256 MiB; run again with a smaller --batch", ""))
            self.log.info("Limits: no answer, or an answer too slow, within the time limit (made 2 s here)")
            assert_equal(module.TIMEOUT, 600)
            module.TIMEOUT = 2
            for server in (RawServer(b"", hold=True), RawServer(b"HTTP/1.1 200 OK\r\n" + b"X: y\r\n" * 1000, drip=True)):
                started = time.time()
                code, out, _ = call_main(module, ["--regtest", "--datadir", self.nodes[0].datadir, "--rpcurl",
                                                  "http://127.0.0.1:%d/" % server.port, "--retries", "0"])
                server.close()
                assert_equal((code, out), ("no answer from 127.0.0.1:%d within 2 s" % server.port, ""))
                assert time.time() - started < 20

        self.log.info("Retries: a full work queue, then a node warming up, then the answer")
        def busy(n, calls):
            if n == 0:
                return (500, b"Work queue depth exceeded")
            if n == 1:
                return (200, json.dumps([{"id": i, "result": None, "error": {"code": -28, "message": "Verifying blocks..."}}
                                         for i in range(len(calls))]).encode())
            return None
        for name, expected in (("p2mr_census.py", self.expected_census(0, TIP)), ("timewarp_count.py", self.expected_count(24, TIP))):
            out, _, proxy = self.proxied(name, [], on_request=busy)
            self.check_report(out, expected)
            assert_equal(proxy.requests[0], proxy.requests[2])
            out, err, _ = self.proxied(name, ["--retries", "1"], rc=1, on_request=busy)
            assert_equal((out, err), ("", "the node is still starting: 'Verifying blocks...'\n"))
            _, err, _ = self.proxied(name, ["--retries", "0"], rc=1, on_request=busy)
            assert re.fullmatch(r"127\.0\.0\.1:[0-9]+ answered with HTTP 500: work queue depth exceeded\.\n", err), err
            _, err, _ = self.proxied(name, [], rc=1, on_request=lambda n, calls: (500, b"Internal error"))
            assert err.endswith("answered with HTTP 500\n")
        self.log.info("Retries: a closed port, with one retry")
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            closed = s.getsockname()[1]
        for name in ("p2mr_census.py", "timewarp_count.py"):
            started = time.time()
            out, err = self.run_script(name, ["--rpcurl", "http://127.0.0.1:%d/" % closed, "--retries", "1"], rc=1)
            assert_equal((out, err), ("", "cannot reach 127.0.0.1:%d.\n" % closed))
            assert 1 <= time.time() - started < 10
            _, err = self.run_script(name, ["--retries", "11"], rc=1)
            assert_equal(err, "--retries must be from 0 to 10\n")
            _, err = self.run_script(name, ["--batch", "0"], rc=1)
            assert_equal(err, "--batch must be from 1 to 500\n")

    def test_partial_nodes(self):
        info = self.nodes[0].getblockchaininfo()
        height_of = {h: i for i, h in enumerate(self.hashes)}

        def node_like(pruneheight=None, ibd=False, headers=0, pruned_heights=()):
            requested = []

            def on_reply(n, calls, replies):
                for call, reply in zip(calls, replies):
                    if call["method"] == "getblockchaininfo":
                        r = reply["result"]
                        r["initialblockdownload"] = ibd
                        r["headers"] += headers
                        if pruneheight is not None:
                            r.update(pruned=True, pruneheight=pruneheight, automatic_pruning=True, prune_target_size=576716800)
                    if call["method"] == "getblock":
                        h = height_of[call["params"][0]]
                        requested.append(h)
                        if (pruneheight is not None and h < pruneheight) or h in pruned_heights:
                            reply.update(result=None, error={"code": -1, "message": "Block not available (pruned data)"})
                return replies
            return on_reply, requested

        def changed(**fields):
            new = dict(info)
            new.update(fields)
            return new

        self.log.info("A pruned node: the census starts at the lowest stored block; the count reads headers")
        pruned_info = changed(pruned=True, pruneheight=300)
        on_reply, requested = node_like(pruneheight=300)
        out, _, _ = self.proxied("p2mr_census.py", [], on_reply=on_reply)
        self.check_report(out, self.expected_census(0, TIP, lowest=300, info=pruned_info,
                                                    notes=["the node is pruned: it has no block below 300, so the scan starts there"]))
        assert_equal(min(requested), 300)
        assert "RESULT: early_coinbase_spends=4 miners=3" in out
        on_reply, requested = node_like(pruneheight=300)
        out, _, _ = self.proxied("timewarp_count.py", [], on_reply=on_reply)
        self.check_report(out, self.expected_count(24, TIP, lowest=300, info=pruned_info, notes=[
            "the node is pruned: it has no block below 300. The counts are complete (they are read from headers); "
            "the miner tag of a block below 300 is \"?\""]))
        assert_greater_than_or_equal(min(requested), 300)
        on_reply, _ = node_like(pruneheight=300)
        _, err, _ = self.proxied("p2mr_census.py", ["--end", "299"], rc=1, on_reply=on_reply)
        assert_equal(err, "the node is pruned: it has no block below 300\n")

        self.log.info("A block pruned during the scan")
        on_reply, _ = node_like(pruned_heights=(400,))
        out, err, _ = self.proxied("p2mr_census.py", [], rc=1, on_reply=on_reply)
        assert_equal((out, err), ("", "the node answered getblock with error -1: 'Block not available (pruned data)'\n"))
        on_reply, _ = node_like(pruned_heights=(223, 500))
        out, _, _ = self.proxied("timewarp_count.py", [], on_reply=on_reply)
        self.check_report(out, self.expected_count(24, TIP, unknown=(223, 500)))

        self.log.info("A node in initial block download, and one with more headers than blocks")
        on_reply, _ = node_like(ibd=True)
        out, _, _ = self.proxied("p2mr_census.py", [], on_reply=on_reply)
        self.check_report(out, self.expected_census(0, TIP, info=changed(initialblockdownload=True),
                                                    notes=["the node is in initial block download"]))
        on_reply, _ = node_like(ibd=True, headers=5)
        out, _, _ = self.proxied("timewarp_count.py", [], on_reply=on_reply)
        self.check_report(out, self.expected_count(24, TIP, info=changed(initialblockdownload=True, headers=TIP + 5), notes=[
            "the node is in initial block download", "the node has 760 blocks but 765 headers"]))

    def alternative_branch(self, fork_height, last_height):
        """Make blocks fork_height..last_height of another branch the active chain; return their hashes."""
        node = self.nodes[0]
        node.invalidateblock(self.hashes[fork_height])
        hashes = {}
        prev = self.hashes[fork_height - 1]
        for h in range(fork_height, last_height + 1):
            block = self.new_block(h, prev, self.times[h] + 1, 0x20000000, b"/branch at %d/" % fork_height, TRUE_SCRIPT)
            self.submit(block)
            hashes[h] = prev = block.hash
        return hashes

    def restore_chain(self, fork_height):
        node = self.nodes[0]
        node.reconsiderblock(self.hashes[fork_height])
        assert_equal(node.getbestblockhash(), self.hashes[TIP])

    def test_reorganisations(self):
        branches = {}

        def reorg_before(method, first_param, fork, last, key):
            def on_request(n, calls):
                if calls[0]["method"] == method and calls[0]["params"][0] == first_param and key not in branches:
                    branches[key] = self.alternative_branch(fork, last)
                return None
            return on_request

        self.log.info("A reorganisation to a shorter chain during a scan")
        out, err, _ = self.proxied("p2mr_census.py", ["--start", "600", "--no-maturity"], rc=1,
                                   on_request=reorg_before("getblockhash", 700, 690, 695, "short"))
        assert_equal((out, err), ("", CHAIN_CHANGED + "\n"))
        self.restore_chain(690)
        self.log.info("A reorganisation between two batches")
        out, err, _ = self.proxied("p2mr_census.py", ["--start", "600", "--end", "700", "--no-maturity"], rc=1,
                                   on_request=reorg_before("getblockhash", 650, 640, 700, "census"))
        assert_equal((out, err), ("", CHAIN_CHANGED + "\n"))
        self.restore_chain(640)
        out, err, _ = self.proxied("timewarp_count.py", ["--start", "24", "--end", "600"], rc=1,
                                   on_request=reorg_before("getblockhash", 501, 495, 600, "count"))
        assert_equal((out, err), ("", CHAIN_CHANGED + "\n"))
        self.restore_chain(495)
        self.log.info("A block of another branch within one batch")
        def swap(height, branch):
            def on_reply(n, calls, replies):
                for call, reply in zip(calls, replies):
                    if call["method"] == "getblockhash" and call["params"][0] == height:
                        reply["result"] = branches[branch][height]
                return replies
            return on_reply
        for height in (660, 650, 699):
            out, err, _ = self.proxied("p2mr_census.py", ["--start", "600", "--end", "700", "--no-maturity"], rc=1,
                                       on_reply=swap(height, "census"))
            assert_equal((out, err), ("", CHAIN_CHANGED + "\n"))
        for height in (550, 501, 600):
            out, err, _ = self.proxied("timewarp_count.py", ["--start", "24", "--end", "600"], rc=1, on_reply=swap(height, "count"))
            assert_equal((out, err), ("", CHAIN_CHANGED + "\n"))
        self.log.info("A best block that is not the block read at its height: the chain changed before it was read")
        def other_best_block(n, calls, replies):
            for call, reply in zip(calls, replies):
                if call["method"] == "getblockchaininfo":
                    reply["result"]["bestblockhash"] = "11" * 32
            return replies
        for name, args in (("p2mr_census.py", ["--start", "700", "--no-maturity"]), ("timewarp_count.py", ["--start", "700"]),
                           ("p2mr_census.py", ["--start", "700", "--end", "720", "--no-maturity"]),
                           ("timewarp_count.py", ["--start", "700", "--end", "720"])):
            out, err, _ = self.proxied(name, args, rc=1, on_reply=other_best_block)
            assert_equal((out, err), ("", CHAIN_CHANGED + "\n"))
        self.log.info("The chain after the reorganisations gives the same reports")
        out, _, _ = self.proxied("timewarp_count.py", [])
        self.check_report(out, self.expected_count(24, TIP))

    def test_interrupted(self):
        self.log.info("An interrupted run prints no result")
        for name in ("p2mr_census.py", "timewarp_count.py"):
            reached, release = threading.Event(), threading.Event()

            def on_request(n, calls):
                if n == 3:
                    reached.set()
                    release.wait(60)
                return None
            proxy = RpcProxy(self.rpc_port)
            proxy.on_request = on_request
            p = subprocess.Popen([sys.executable, "-I", self.script(name), "--regtest", "--datadir", self.nodes[0].datadir,
                                  "--rpcurl", proxy.url(), "--batch", "10"], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, start_new_session=True)
            assert reached.wait(60)
            p.send_signal(signal.SIGINT)
            out, err = p.communicate(timeout=60)
            release.set()
            proxy.close()
            assert_equal((p.returncode, out, err), (1, b"", b"interrupted; no result. Run the script again.\n"))

    def test_mweb_history_check(self):
        node = self.nodes[0]
        name = "mweb_history_check.py"
        args = ["--datadir", os.path.join(node.datadir, "regtest"), "--rpcurl", self.node_url()]
        info = node.getblockchaininfo()
        mweb = info["softforks"].get("mweb", {})
        self.log.info("mweb_history_check.py: the normal result")
        assert not mweb.get("active")
        # (After invalidateblock and reconsiderblock the node's best header can be a lower one.)
        first = "chain=regtest blocks=%d headers=%d mweb_active=%s mweb_height=None\n" % (TIP, info["headers"], mweb.get("active"))
        not_active = "MWEB is not active on this chain; nothing to check.\n"
        out, err = self.run_script(name, args, rc=1)
        assert_equal((out, err), (first, not_active))
        self.log.info("mweb_history_check.py: proxy variables of the environment are not used")
        listener = Listener()
        env = {v: "http://127.0.0.1:%d/" % listener.port for v in PROXY_VARIABLES}
        out, err = self.run_script(name, args, env=env, rc=1)
        assert_equal((out, err), (first, not_active))
        assert_equal(listener.accepted, 0)
        self.log.info("mweb_history_check.py: redirects are reported, not followed")
        for status in (301, 302, 303, 307, 308):
            proxy = RpcProxy(self.rpc_port, redirect=(status, "http://127.0.0.1:%d/" % listener.port))
            out, err = self.run_script(name, args[:2] + ["--rpcurl", proxy.url()], rc=1)
            proxy.close()
            assert_equal((out, err), ("", "127.0.0.1:%d answered with HTTP %d, a redirect, which is not followed.\n" % (proxy.port, status)))
            assert_equal(len(proxy.requests), 1)
        assert_equal(listener.accepted, 0)
        self.log.info("mweb_history_check.py: URLs other than http://HOST:PORT/ are refused before any request")
        self.check_refused_urls(name, listener)
        listener.close()
        self.log.info("mweb_history_check.py: a closed port ends with one line")
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            closed = s.getsockname()[1]
        module = import_script(self.script(name), "mweb_history_check")
        assert_equal(module.RETRY_WAITS, [1, 2, 4, 8])
        module.RETRY_WAITS = [0.1, 0.1, 0.1, 0.1]
        code, out, err = call_main(module, args[:2] + ["--rpcurl", "http://127.0.0.1:%d/" % closed])
        assert_equal((code, out, err), ("cannot reach 127.0.0.1:%d." % closed, "", ""))
        code, out, err = call_main(module, args[:2] + ["--rpcuser", "u", "--rpcpassword", "p", "--rpcurl", self.node_url()])
        assert_equal(code, "RPC authentication failed (HTTP 401): check --datadir or --rpcuser/--rpcpassword.")

    def test_headers_ahead(self):
        self.log.info("A node with a header whose block it does not have")
        node = self.nodes[0]
        block = self.new_block(TIP + 1, self.hashes[TIP], self.times[TIP] + 60, 0x20000000, b"/header-only/", TRUE_SCRIPT)
        node.submitheader(block.serialize()[:80].hex())
        info = node.getblockchaininfo()
        assert_equal((info["blocks"], info["headers"]), (TIP, TIP + 1))
        out, _ = self.run_script("p2mr_census.py", ["--start", "700", "--rpcurl", self.node_url()])
        self.check_report(out, self.expected_census(700, TIP, info=info, notes=[
            "the range does not cover the whole chain (0-760)", "the node has 760 blocks but 761 headers"]))
        out, _ = self.run_script("timewarp_count.py", ["--rpcurl", self.node_url()])
        self.check_report(out, self.expected_count(24, TIP, info=info, notes=["the node has 760 blocks but 761 headers"]))

    def test_mweb_node(self):
        if not self.is_wallet_compiled():
            self.log.info("No wallet: the MWEB chain is not built")
            return
        self.log.info("A node with MWEB data on its chain")
        self.start_node(1)
        mweb_node = self.nodes[1]
        mweb_node.createwallet(wallet_name="mweb")
        mweb_node.importprivkey(privkey=mweb_node.get_deterministic_priv_key().key, label="coinbase")
        setup_mweb_chain(mweb_node)
        mweb_node.sendtoaddress(mweb_node.getnewaddress(address_type="mweb"), 2)
        mweb_node.generate(5)
        tip = mweb_node.getblockcount()
        info = mweb_node.getblockchaininfo()
        port = rpc_port(1)
        url = "http://127.0.0.1:%d/" % port
        out, _ = self.run_script("mweb_history_check.py", ["--datadir", os.path.join(mweb_node.datadir, "regtest"), "--rpcurl", url])
        assert_equal(out.splitlines()[-1], "RESULT: no M1/M2 violations (heights %d-%d)." % (info["softforks"]["mweb"]["height"], tip))
        mweb_blocks = int(re.search(r"mweb_blocks_with_data +([0-9,]+)", out).group(1).replace(",", ""))
        assert_greater_than_or_equal(mweb_blocks, 1)
        # The census skips MWEB inputs and outputs and counts the early coinbase spends of the wallet,
        # which the test finds here from the blocks themselves.
        coinbases, early = {}, 0
        for h in range(tip + 1):
            block = mweb_node.getblock(mweb_node.getblockhash(h), 2)
            for i, tx in enumerate(block["tx"]):
                for txin in tx["vin"]:
                    if i and not txin["ismweb"] and txin["txid"] in coinbases and 10 <= h - coinbases[txin["txid"]] <= 99:
                        early += 1
            coinbases[block["tx"][0]["txid"]] = h
        out, _ = self.run_script("p2mr_census.py", ["--regtest", "--datadir", mweb_node.datadir, "--rpcurl", url], base_args=False)
        results = [l for l in out.splitlines() if l.startswith("RESULT: ")]
        assert_equal(results[0], "RESULT: v2_32_unspent=0 value=0.00000000")
        assert_equal(results[4].split(" miners=")[0], "RESULT: early_coinbase_spends=%d" % early)
        assert_equal(results[5], "RESULT: range=0-%d complete" % tip)
        out, _ = self.run_script("timewarp_count.py", ["--regtest", "--datadir", mweb_node.datadir, "--rpcurl", url], base_args=False)
        assert_equal(out.splitlines()[-1], "RESULT: range=24-%d complete" % tip)


if __name__ == '__main__':
    CountScriptsTest().main()
