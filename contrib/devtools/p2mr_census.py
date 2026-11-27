#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Read-only census of witness version 2 outputs and of early coinbase spends.

It reads every block of the range from a Ferrite Core node (v3.3.0 or later) and reports:
  (a) witness version 2 outputs: scriptPubKey OP_2 followed by one direct push of 2 to 40 bytes.
      Every unspent output with a 32-byte program (the P2MR form of BIP 360) is listed by value,
      every spent one with the block and transaction that spent it and whether its witness has
      the structure BIP 360 requires. Other program lengths are counted. Spends are followed in
      the blocks read, not in the node's UTXO set; MWEB inputs and outputs are skipped.
  (b) spends of coinbase outputs 10 to 99 blocks after the block that created them, which the
      coinbasematurity deployment refuses for coinbases created once it is active, by miner
      tag and by address (off with --no-maturity);
  and, for each signalling window, the blocks that signal bits 6, 7 and 8.

The script calls only getblockchaininfo, getblockhash and getblock. It changes nothing and writes
no file: the report goes to standard output and progress to standard error. It connects only to
the host and port of --rpcurl: no proxy is used and no redirect is followed. Credentials are taken
from --rpcuser/--rpcpassword, else (for a node on this computer only) from the .cookie file or the
rpcuser and rpcpassword lines of ferrite.conf in the data directory, else asked for.

Usage (Python 3.8+, standard library only), on the computer that runs the node (server=1):
  python3 contrib/devtools/p2mr_census.py > census.txt
  python3 contrib/devtools/p2mr_census.py --datadir /path/to/datadir --start 1050000
  python  contrib\\devtools\\p2mr_census.py --testnet
The last lines start with "RESULT: " and summarise the report.
"""
import argparse
import base64
import collections
import decimal
import getpass
import hashlib
import http.client
import ipaddress
import json
import os
import re
import socket
import sys
import threading
import time
import urllib.parse

ALLOWED_METHODS = frozenset({"getblockchaininfo", "getblockhash", "getblock"})

# --- Common part: the same in p2mr_census.py and timewarp_count.py ---

NOT_A_NODE = "did not answer as the RPC server of a Ferrite Core node does"
CHAIN_CHANGED = "the chain changed during the scan. Run the script again."
NO_TLS = "ferrited serves no TLS; use http:// (through an SSH tunnel for a remote node)"
BAD_URL = "--rpcurl must have the form http://HOST:PORT/"
AUTH_FAILED = "RPC authentication failed (HTTP 401): check --datadir or --rpcuser/--rpcpassword."
AUTH_FAILED_LATER = ("RPC authentication failed (HTTP 401) during the scan: the node may have restarted and written a "
                     "new cookie. Run the script again.")
TIMEOUT = 600
MAX_REPLY_BYTES = 256 * 1024 * 1024
RETRY_WAITS = [1, 2, 4, 8]
MAX_MONEY = 60221400 * 100000000
HEX64 = re.compile(r"[0-9a-f]{64}\Z")
HEX = re.compile(r"(?:[0-9a-f]{2})*\Z")
NAME = re.compile(r"[a-z0-9_]{1,32}\Z")
HOST_LABEL = re.compile(r"[a-z0-9-]{1,63}\Z")
# chain name -> (signalling window, bech32 prefix, base58 versions of P2PKH and P2SH addresses)
CHAINS = {
    "main": (40, "fec", 36, 35),
    "test": (20, "tfec", 111, 35),
    "regtest": (144, "rfec", 111, 58),
}


def fail(message):
    """End the script with a message that holds nothing taken unchecked from a reply."""
    sys.exit(message)


def want(condition):
    if not condition:
        fail(NOT_A_NODE)


def clean(text, limit):
    """Free text from a reply or an exception: printable ASCII only, cut to limit characters."""
    if not isinstance(text, str):
        text = str(text)
    return "".join(c for c in text if " " <= c <= "~")[:limit]


def is_int(value, low, high):
    return type(value) is int and low <= value <= high


def checked_hash(value):
    want(isinstance(value, str) and HEX64.match(value) is not None)
    return value


def checked_hex(value, max_digits):
    want(isinstance(value, str) and len(value) <= max_digits and HEX.match(value) is not None)
    return value


def checked_amount(value):
    """An amount in FEC, read as an exact decimal; returned in units of 1e-8 FEC."""
    # The exponent is bounded first, so that the product below cannot overflow.
    want(isinstance(value, decimal.Decimal) and value.is_finite() and value.adjusted() < 12)
    units = value * 100000000
    want(units == units.to_integral_value() and 0 <= units <= MAX_MONEY)
    return int(units)


def fec(units):
    return "%d.%08d" % divmod(units, 100000000)


def miner_tag(coinbase_hex):
    """The readable text of a coinbase input script after the height: runs of 4 or more printable
    characters, joined by spaces, cut to 60 characters."""
    data = bytes.fromhex(coinbase_hex)
    if data and 1 <= data[0] <= 75:
        data = data[1 + data[0]:]
    elif data and (data[0] == 0 or 0x51 <= data[0] <= 0x60):
        data = data[1:]
    runs = re.findall(rb"[\x20-\x7e]{4,}", data)
    return clean(" ".join(run.decode("ascii") for run in runs).strip(), 60)


def is_host_name(host):
    """A host name the socket module can look up: labels of 1 to 63 letters, digits and hyphens, at most 253
    characters, a final dot allowed."""
    labels = (host[:-1] if host.endswith(".") else host).split(".")
    return len(host) <= 253 and all(HOST_LABEL.match(label) for label in labels)


def parse_rpcurl(url):
    """Return (host, port) of an http://HOST:PORT/ URL. A refused URL is never printed."""
    if not re.match(r"[!-~]+\Z", url):
        fail(BAD_URL)
    try:
        parts = urllib.parse.urlsplit(url)
        port = parts.port
    except ValueError:
        fail(BAD_URL)
    if parts.scheme == "https":
        fail(NO_TLS)
    if parts.scheme != "http" or "@" in parts.netloc or "?" in url or "#" in url:
        fail(BAD_URL)
    if parts.path not in ("", "/") or port is None or not 1 <= port <= 65535:
        fail(BAD_URL)
    host = parts.hostname or ""
    if parts.netloc.startswith("["):
        try:
            host = str(ipaddress.IPv6Address(host))
        except ValueError:
            fail(BAD_URL)
    elif not is_host_name(host):
        fail(BAD_URL)
    return host, port


def is_loopback(host):
    if host == "localhost":
        return True
    try:
        address = ipaddress.ip_address(host)
    except ValueError:
        return False
    return (getattr(address, "ipv4_mapped", None) or address).is_loopback


def default_datadir():
    if sys.platform == "win32":
        return os.path.join(os.environ.get("APPDATA", ""), "Ferrite")
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Application Support/Ferrite")
    return os.path.expanduser("~/.ferrite")


def conf_credentials(path, section):
    """rpcuser and rpcpassword from ferrite.conf, read as the node reads them: the chain's section
    first, then the lines above the first section; the first value of each wins."""
    found = {}
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return None
    prefix = ""
    for line in lines:
        line = line.split("#", 1)[0].strip(" \t\r\n")
        if line.startswith("[") and line.endswith("]"):
            prefix = line[1:-1] + "."
        elif "=" in line:
            key, value = line.split("=", 1)
            found.setdefault(prefix + key.strip(" \t\r\n"), value.strip(" \t\r\n"))
    result = []
    for key in ("rpcuser", "rpcpassword"):
        value = found.get(section + "." + key, found.get(key))
        if value is None:
            return None
        result.append(value)
    return tuple(result)


def credentials(args, host, port):
    """Return (user, password). Local credentials are read only for a loopback host."""
    if args.rpcpassword is not None and args.rpcuser is None:
        fail("--rpcpassword needs --rpcuser")
    if args.rpcuser is not None:
        password = args.rpcpassword
        if password is None:
            password = getpass.getpass("RPC password for %s: " % args.rpcuser)
        return args.rpcuser, password
    if args.regtest:
        subdir, section = "regtest", "regtest"
    elif args.testnet or port == 19573:
        subdir, section = "testnet4", "test"
    else:
        subdir, section = "", "main"
    if is_loopback(host):
        datadir = args.datadir if args.datadir is not None else default_datadir()
        cookie_dir = os.path.join(datadir, subdir) if subdir else datadir
        try:
            with open(os.path.join(cookie_dir, ".cookie"), encoding="ascii") as f:
                user, sep, password = f.read().strip().partition(":")
            if sep:
                return user, password
        except (OSError, ValueError):
            pass
        found = conf_credentials(os.path.join(datadir, "ferrite.conf"), section)
        if found is not None:
            return found
        print("no .cookie in %s and no rpcuser in ferrite.conf" % cookie_dir, file=sys.stderr)
    sys.stderr.write("RPC user: ")
    sys.stderr.flush()
    user = sys.stdin.readline().strip()
    return user, getpass.getpass("RPC password: ")


class NodeRpc:
    """Sends read-only JSON-RPC batches to one host and port over plain HTTP. No proxy is used, no
    redirect is followed, and only the methods in ALLOWED_METHODS, with checked parameters, are sent."""

    def __init__(self, host, port, user, password, retries):
        self.host = host
        self.port = port
        self.retries = retries
        token = base64.b64encode(("%s:%s" % (user, password)).encode("utf-8")).decode("ascii")
        self._headers = {"Authorization": "Basic " + token, "Content-Type": "application/json"}
        self.seconds = {}
        # A node that restarts writes a new cookie, so a refusal after an accepted request is reported as such.
        self._cookie = user == "__cookie__"
        self._accepted = False

    @staticmethod
    def check_call(method, params):
        if method not in ALLOWED_METHODS:
            fail("refused to send %r: this script sends only %s" % (clean(method, 40), ", ".join(sorted(ALLOWED_METHODS))))
        if method == "getblockchaininfo":
            ok = params == []
        elif method == "getblockhash":
            ok = len(params) == 1 and is_int(params[0], 0, 2**31 - 1)
        elif method == "getblock":
            ok = (len(params) == 2 and isinstance(params[0], str) and HEX64.match(params[0]) is not None
                  and is_int(params[1], 2, 2))
        elif method == "getblockheader":
            ok = (len(params) == 2 and isinstance(params[0], str) and HEX64.match(params[0]) is not None
                  and params[1] is True)
        else:
            ok = False
        if not ok:
            fail("refused to send %s with parameters this script does not use" % method)

    def batch(self, calls):
        """Send [(method, params), ...]; return [(result, error), ...] in the same order."""
        for method, params in calls:
            self.check_call(method, params)
        body = json.dumps([{"jsonrpc": "1.0", "id": i, "method": m, "params": p}
                           for i, (m, p) in enumerate(calls)]).encode("ascii")
        started = time.monotonic()
        attempt = 0
        while True:
            outcome, data = self._post(body)
            if outcome == "unreachable":
                reason = "cannot reach %s:%d." % (self.host, self.port)
            elif outcome == "busy":
                reason = "%s:%d answered with HTTP 500: work queue depth exceeded." % (self.host, self.port)
            else:
                replies = self._parse(data, len(calls))
                self._accepted = True
                if not all(error is not None and error["code"] == -28 for _, error in replies):
                    key = calls[0][0]
                    self.seconds[key] = self.seconds.get(key, 0.0) + time.monotonic() - started
                    return replies
                reason = "the node is still starting: %r" % clean(replies[0][1]["message"], 200)
            if attempt >= self.retries:
                fail(reason)
            time.sleep(RETRY_WAITS[min(attempt, len(RETRY_WAITS) - 1)])
            attempt += 1

    def _post(self, body):
        """Return ("ok", body) for an answer with HTTP 200, ("busy", None) when the node's work queue is
        full and ("unreachable", None) for a connection error; end the script for any other answer."""
        conn = http.client.HTTPConnection(self.host, self.port, timeout=TIMEOUT)
        expired = []

        def expire():
            expired.append(True)
            try:
                conn.sock.shutdown(socket.SHUT_RDWR)
            except (AttributeError, OSError):
                pass

        timer = threading.Timer(TIMEOUT, expire)
        timer.daemon = True
        timer.start()
        try:
            conn.request("POST", "/", body, self._headers)
            response = conn.getresponse()
            status = response.status
            if status == 200:
                length = response.getheader("Content-Length")
                if length is not None and length.isdigit() and int(length) > MAX_REPLY_BYTES:
                    fail("a reply was larger than 256 MiB; run again with a smaller --batch")
                data = response.read(MAX_REPLY_BYTES + 1)
                if len(data) > MAX_REPLY_BYTES:
                    fail("a reply was larger than 256 MiB; run again with a smaller --batch")
                if expired:
                    fail("no answer from %s:%d within %d s" % (self.host, self.port, TIMEOUT))
                return "ok", data
            if status == 401:
                fail(AUTH_FAILED_LATER if self._cookie and self._accepted else AUTH_FAILED)
            if 300 <= status < 400:
                fail("%s:%d answered with HTTP %d, a redirect, which is not followed"
                     % (self.host, self.port, status))
            if status == 500 and response.read(64) == b"Work queue depth exceeded":
                return "busy", None
            fail("%s:%d answered with HTTP %d" % (self.host, self.port, status))
        except socket.timeout:
            fail("no answer from %s:%d within %d s" % (self.host, self.port, TIMEOUT))
        except (http.client.RemoteDisconnected, ConnectionError):
            if expired:
                fail("no answer from %s:%d within %d s" % (self.host, self.port, TIMEOUT))
            return "unreachable", None
        except http.client.HTTPException:
            if expired:
                fail("no answer from %s:%d within %d s" % (self.host, self.port, TIMEOUT))
            fail(NOT_A_NODE)
        except OSError:
            if expired:
                fail("no answer from %s:%d within %d s" % (self.host, self.port, TIMEOUT))
            return "unreachable", None
        finally:
            timer.cancel()
            conn.close()

    @staticmethod
    def _parse(data, count):
        def refuse_constant(name):
            raise ValueError(name)

        try:
            replies = json.loads(data.decode("utf-8"), parse_float=decimal.Decimal, parse_constant=refuse_constant)
        except (ValueError, RecursionError):
            fail(NOT_A_NODE)
        want(isinstance(replies, list) and len(replies) == count)
        by_id = {}
        for reply in replies:
            want(isinstance(reply, dict) and "result" in reply and "error" in reply)
            want(is_int(reply.get("id"), 0, count - 1) and reply["id"] not in by_id)
            error = reply["error"]
            if error is not None:
                want(isinstance(error, dict) and is_int(error.get("code"), -2**31, 2**31 - 1)
                     and isinstance(error.get("message"), str))
            by_id[reply["id"]] = (reply["result"], error)
        return [by_id[i] for i in range(count)]

    def results(self, method, params_list, on_error=None):
        """Call one method for each parameter list; a node error ends the script unless on_error
        gives a value for it."""
        out = []
        for result, error in self.batch([(method, params) for params in params_list]):
            if error is not None:
                if on_error is not None:
                    value = on_error(error)
                    if value is not None:
                        out.append(value)
                        continue
                fail("the node answered %s with error %d: %r" % (method, error["code"], clean(error["message"], 200)))
            out.append(result)
        return out


class ArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        # argparse's own message repeats the arguments, which may hold a password.
        self.print_usage(sys.stderr)
        self.exit(2, "%s: invalid or unrecognised arguments; see --help\n" % self.prog)


def common_arguments(parser):
    parser.add_argument("--rpcurl", help="the node's RPC address (default: http://127.0.0.1:9573/, "
                        "with --testnet http://127.0.0.1:19573/)")
    parser.add_argument("--testnet", action="store_true", help="the node runs the test network")
    parser.add_argument("--regtest", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--datadir", help="the node's data directory, for the cookie or ferrite.conf "
                        "(default: the platform's data directory)")
    parser.add_argument("--rpcuser", help="RPC user name (shown in the process list)")
    parser.add_argument("--rpcpassword", help="RPC password (shown in the process list; without it, "
                        "the script asks for it)")
    parser.add_argument("--start", type=int, help="first height")
    parser.add_argument("--end", type=int, help="last height (default: the tip)")
    parser.add_argument("--retries", type=int, default=4, help="retries after a failed request, "
                        "0 to 10 (default: %(default)s)")


def connect(args):
    """Check the options, connect and return (rpc, getblockchaininfo result)."""
    if not 0 <= args.retries <= 10:
        fail("--retries must be from 0 to 10")
    if not 1 <= args.batch <= 500:
        fail("--batch must be from 1 to 500")
    if args.testnet and args.regtest:
        fail("--testnet and --regtest cannot be used together")
    url = args.rpcurl
    if url is None:
        url = "http://127.0.0.1:%d/" % (19443 if args.regtest else 19573 if args.testnet else 9573)
    host, port = parse_rpcurl(url)
    user, password = credentials(args, host, port)
    rpc = NodeRpc(host, port, user, password, args.retries)
    info = rpc.results("getblockchaininfo", [[]])[0]
    want(isinstance(info, dict) and isinstance(info.get("chain"), str) and info["chain"] in CHAINS)
    want(is_int(info.get("blocks"), 0, 2**31 - 1) and is_int(info.get("headers"), -1, 2**31 - 1))
    checked_hash(info.get("bestblockhash"))
    want(type(info.get("initialblockdownload")) is bool and type(info.get("pruned")) is bool)
    if info["pruned"]:
        want(is_int(info.get("pruneheight"), 0, info["blocks"]))
    softforks = info.get("softforks")
    want(isinstance(softforks, dict))
    for name, fork in softforks.items():
        want(NAME.match(name) is not None and isinstance(fork, dict) and fork.get("type") in ("buried", "bip8", "bip9"))
        want(type(fork.get("active")) is bool)
        if "height" in fork:
            want(is_int(fork["height"], 0, 2**31 - 1))
        if fork["type"] != "buried":
            want(isinstance(fork.get(fork["type"]), dict)
                 and fork[fork["type"]].get("status") in ("defined", "started", "locked_in", "active", "failed"))
    return rpc, info


def softfork_lines(info):
    lines = []
    for name in sorted(info["softforks"]):
        fork = info["softforks"][name]
        status = "buried" if fork["type"] == "buried" else fork[fork["type"]]["status"]
        lines.append("  %s type=%s status=%s active=%s height=%s" % (
            name, fork["type"], status, "yes" if fork["active"] else "no", fork.get("height", "-")))
    return lines


def block_hashes(rpc, low, high):
    """The hashes of the active chain at heights low..high."""
    def out_of_range(error):
        if error["code"] == -8:
            fail(CHAIN_CHANGED)
        return None

    return [checked_hash(h) for h in rpc.results("getblockhash", [[h] for h in range(low, high + 1)], out_of_range)]


def check_best_block(rpc, info, last, last_hash):
    """Check that the blocks read, up to last_hash at height last, are on the chain whose best block
    getblockchaininfo named: the block at its height must still be that block."""
    tip = info["blocks"]
    tip_hash = last_hash if last == tip else block_hashes(rpc, tip, tip)[0]
    if tip_hash != info["bestblockhash"]:
        fail(CHAIN_CHANGED)


def check_link(item, height, block_hash, previous_hash):
    """Check the height, hash and parent of a block or header read at height after previous_hash."""
    want(isinstance(item, dict) and item.get("hash") == block_hash)
    want(is_int(item.get("height"), 0, 2**31 - 1))
    want(is_int(item.get("time"), 0, 2**32 - 1) and is_int(item.get("version"), -2**31, 2**32 - 1))
    want(item["height"] == height)
    if height > 0:
        checked_hash(item.get("previousblockhash"))
        if previous_hash is not None and item["previousblockhash"] != previous_hash:
            fail(CHAIN_CHANGED)


def coinbase_hex(block):
    """The input script of the block's coinbase transaction."""
    txs = block.get("tx")
    want(isinstance(txs, list) and txs and isinstance(txs[0], dict) and isinstance(txs[0].get("vin"), list))
    want(txs[0]["vin"] and isinstance(txs[0]["vin"][0], dict))
    return checked_hex(txs[0]["vin"][0].get("coinbase"), 200)


def report_rpc_time(rpc, started):
    parts = ", ".join("%s %.1f s" % (m, s) for m, s in sorted(rpc.seconds.items()))
    print("finished in %.1f s (time in RPC: %s)" % (time.monotonic() - started, parts), file=sys.stderr)


def run(main):
    try:
        main()
    except KeyboardInterrupt:
        sys.exit("interrupted; no result. Run the script again.")
    except EOFError:
        sys.exit("no RPC password given")
    except BrokenPipeError:
        sys.exit(1)

# --- End of the common part ---


MATURITY_OLD = 10
MATURITY_NEW = 100
BECH32_CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"
BASE58_CHARSET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def tagged_hash(tag, data):
    tag_hash = hashlib.sha256(tag.encode("ascii")).digest()
    return hashlib.sha256(tag_hash + tag_hash + data).digest()


def compact_size(n):
    if n < 253:
        return bytes([n])
    if n <= 0xffff:
        return b"\xfd" + n.to_bytes(2, "little")
    if n <= 0xffffffff:
        return b"\xfe" + n.to_bytes(4, "little")
    return b"\xff" + n.to_bytes(8, "little")


def bip360_structure(program, witness):
    """Check the witness of a spend of a witness version 2 output with the 32-byte program against
    the structure rules of BIP 360 (steps 2 to 10 of its script validation). Return "valid" or the
    step that fails. The leaf script is not executed."""
    stack = [bytes.fromhex(item) for item in witness]
    if not stack:
        return "step2_empty_witness"
    if len(stack) == 1:
        return "step2_one_element"
    if stack[-1][:1] == b"\x50":
        if len(stack) == 2:
            return "step3_annex_without_script"
        stack = stack[:-1]
    control, script = stack[-1], stack[-2]
    if not (1 <= len(control) <= 4097 and (len(control) - 1) % 32 == 0):
        return "step6_control_block_size"
    if control[0] & 1 != 1:
        return "step7_control_block_parity"
    k = tagged_hash("TapLeaf", bytes([control[0] & 0xfe]) + compact_size(len(script)) + script)
    for j in range((len(control) - 1) // 32):
        e = control[1 + 32 * j:33 + 32 * j]
        k = tagged_hash("TapBranch", k + e if k < e else e + k)
    if k != program:
        return "step10_root_differs"
    return "valid"


def witness_program(script):
    """(version, program) if script is a witness program (as CScript::IsWitnessProgram), else None."""
    if 4 <= len(script) <= 42 and (script[0] == 0 or 0x51 <= script[0] <= 0x60) and script[1] == len(script) - 2:
        return (0 if script[0] == 0 else script[0] - 0x50), script[2:]
    return None


def bech32_address(hrp, version, program):
    values, acc, bits = [version], 0, 0
    for byte in program:
        acc = (acc << 8) | byte
        bits += 8
        while bits >= 5:
            bits -= 5
            values.append((acc >> bits) & 31)
    if bits:
        values.append((acc << (5 - bits)) & 31)
    check = 1
    for v in [ord(c) >> 5 for c in hrp] + [0] + [ord(c) & 31 for c in hrp] + values + [0] * 6:
        top = check >> 25
        check = (check & 0x1ffffff) << 5 ^ v
        for i, g in enumerate((0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3)):
            if (top >> i) & 1:
                check ^= g
    check ^= 1 if version == 0 else 0x2bc830a3
    values += [(check >> 5 * (5 - i)) & 31 for i in range(6)]
    return hrp + "1" + "".join(BECH32_CHARSET[v] for v in values)


def base58_address(version, payload):
    data = bytes([version]) + payload
    data += hashlib.sha256(hashlib.sha256(data).digest()).digest()[:4]
    n, text = int.from_bytes(data, "big"), ""
    while n:
        n, r = divmod(n, 58)
        text = BASE58_CHARSET[r] + text
    return "1" * (len(data) - len(data.lstrip(b"\0"))) + text


def address(script_hex, chain):
    """The address of a validated scriptPubKey, derived here; "script:<hex>" when it has none."""
    _, hrp, p2pkh, p2sh = CHAINS[chain]
    script = bytes.fromhex(script_hex)
    if len(script) == 25 and script[:3] == b"\x76\xa9\x14" and script[23:] == b"\x88\xac":
        return base58_address(p2pkh, script[3:23])
    if len(script) == 23 and script[:2] == b"\xa9\x14" and script[22] == 0x87:
        return base58_address(p2sh, script[2:22])
    program = witness_program(script)
    if program is not None and (program[0] != 0 or len(program[1]) in (20, 32)):
        return bech32_address(hrp, program[0], program[1])
    return "script:" + (script_hex if len(script_hex) <= 80 else script_hex[:80] + "...")


class Census:
    def __init__(self, chain, first, maturity):
        self.chain = chain
        self.window = CHAINS[chain][0]
        self.first = first
        self.maturity = maturity
        self.unspent = {}       # outpoint -> output record, v2 with a 32-byte program
        self.spent = []
        self.other = {}         # outpoint -> output record, v2 with another program length
        self.coinbases = {}     # txid -> (height, miner tag, [(value, scriptPubKey hex)])
        self.recent = collections.deque()
        self.early = []
        self.signals = {}       # window index -> [bit 6, bit 7, bit 8]

    def visit(self, height, block):
        counted = height >= self.first
        tag = miner_tag(coinbase_hex(block))
        version = block["version"] & 0xffffffff
        if counted and version & 0xe0000000 == 0x20000000:
            for i, bit in enumerate((6, 7, 8)):
                if version >> bit & 1:
                    self.signals.setdefault(height // self.window, [0, 0, 0])[i] += 1
        for index, tx in enumerate(block["tx"]):
            want(isinstance(tx, dict) and isinstance(tx.get("vin"), list) and isinstance(tx.get("vout"), list))
            txid = checked_hash(tx.get("txid"))
            for txin in tx["vin"]:
                want(isinstance(txin, dict) and type(txin.get("ismweb")) is bool)
                if index == 0:
                    want("coinbase" in txin)
                    continue
                if txin["ismweb"] or not counted:
                    continue
                outpoint = (checked_hash(txin.get("txid")), txin.get("vout"))
                want(is_int(outpoint[1], 0, 2**32 - 1))
                witness = txin.get("txinwitness", [])
                want(isinstance(witness, list))
                if outpoint in self.unspent:
                    record = self.unspent.pop(outpoint)
                    record["spent_height"] = height
                    record["spent_txid"] = txid
                    record["bip360"] = bip360_structure(bytes.fromhex(record["script"][4:]),
                                                        [checked_hex(item, 2**28) for item in witness])
                    self.spent.append(record)
                elif outpoint in self.other:
                    self.other[outpoint]["spent_height"] = height
                if self.maturity and outpoint[0] in self.coinbases:
                    created, creator, outputs = self.coinbases[outpoint[0]]
                    want(outpoint[1] < len(outputs))
                    depth = height - created
                    if MATURITY_OLD <= depth < MATURITY_NEW:
                        value, script = outputs[outpoint[1]]
                        self.early.append(dict(height=height, depth=depth, txid=txid, outpoint=outpoint,
                                               created=created, value=value, miner=creator,
                                               address=address(script, self.chain)))
            outputs = []
            for txout in tx["vout"]:
                want(isinstance(txout, dict) and type(txout.get("ismweb")) is bool)
                if txout["ismweb"]:
                    continue
                want(txout.get("n") == len(outputs) and isinstance(txout.get("scriptPubKey"), dict))
                value = checked_amount(txout.get("value"))
                script = checked_hex(txout["scriptPubKey"].get("hex"), 20000)
                outputs.append((value, script))
                program = witness_program(bytes.fromhex(script)) if script[:2] == "52" else None
                if counted and program is not None:
                    record = dict(height=height, value=value, coinbase=index == 0, script=script,
                                  outpoint=(txid, txout["n"]), length=len(program[1]))
                    (self.unspent if len(program[1]) == 32 else self.other)[(txid, txout["n"])] = record
            if index == 0:
                self.coinbases[txid] = (height, tag, outputs)
                self.recent.append((height, txid))
        while self.recent and self.recent[0][0] <= height - MATURITY_NEW:
            self.coinbases.pop(self.recent.popleft()[1], None)

    def window_name(self, index):
        return "%d-%d" % (index * self.window, (index + 1) * self.window - 1)


def early_line(e):
    return ("  spend_height=%d depth=%d txid=%s coinbase=%s:%d created=%d value=%s address=%s miner=%r"
            % (e["height"], e["depth"], e["txid"], e["outpoint"][0], e["outpoint"][1], e["created"],
               fec(e["value"]), e["address"], e["miner"]))


def main():
    parser = ArgumentParser(prog="p2mr_census.py", allow_abbrev=False, description=__doc__,
                            formatter_class=argparse.RawDescriptionHelpFormatter)
    common_arguments(parser)
    parser.add_argument("--batch", type=int, default=50, help="blocks per request, 1 to 500 (default: %(default)s)")
    parser.add_argument("--no-maturity", action="store_true", help="leave out the early coinbase spends")
    args = parser.parse_args()
    started = time.monotonic()
    rpc, info = connect(args)
    chain, tip = info["chain"], info["blocks"]
    first = 0 if args.start is None else args.start
    last = tip if args.end is None else args.end
    if not 0 <= first <= last <= tip:
        fail("--start and --end must give 0 <= start <= end <= %d (the tip)" % tip)
    notes = []
    if first > 0 or last < tip:
        notes.append("the range does not cover the whole chain (0-%d)" % tip)
    lowest = info["pruneheight"] if info["pruned"] else 0
    if lowest > first:
        if lowest > last:
            fail("the node is pruned: it has no block below %d" % lowest)
        notes.append("the node is pruned: it has no block below %d, so the scan starts there" % lowest)
        first = lowest
    if info["initialblockdownload"]:
        notes.append("the node is in initial block download")
    if tip < info["headers"]:
        notes.append("the node has %d blocks but %d headers" % (tip, info["headers"]))
    read_from = first if args.no_maturity else max(lowest, first - (MATURITY_NEW - 1))
    remarks = []
    if read_from < first:
        remarks.append("blocks %d-%d are read only for the coinbases that blocks of the range spend"
                       % (read_from, first - 1))

    census = Census(chain, first, not args.no_maturity)
    previous = None
    shown = time.monotonic()
    for low in range(read_from, last + 1, args.batch):
        high = min(low + args.batch - 1, last)
        hashes = block_hashes(rpc, low, high)
        blocks = rpc.results("getblock", [[h, 2] for h in hashes])
        for height, block_hash, block in zip(range(low, high + 1), hashes, blocks):
            check_link(block, height, block_hash, previous)
            census.visit(height, block)
            previous = block_hash
        if time.monotonic() - shown >= 10:
            shown = time.monotonic()
            print("  block %d of %d (%.0f blocks/s)" % (high, last, (high - read_from + 1) / (shown - started)),
                  file=sys.stderr)
    check_best_block(rpc, info, last, previous)

    out = ["p2mr_census.py: witness version 2 outputs and early coinbase spends",
           "chain=%s blocks=%d headers=%d bestblockhash=%s initialblockdownload=%s pruned=%s" % (
               chain, tip, info["headers"], info["bestblockhash"], "yes" if info["initialblockdownload"] else "no",
               "yes (pruneheight %d)" % lowest if info["pruned"] else "no"),
           "softforks:"] + softfork_lines(info)
    complete = not notes
    out.append("range=%d-%d %s" % (first, last, "complete" if complete else "partial"))
    out += ["note: " + n for n in notes + remarks]

    unspent = sorted(census.unspent.values(), key=lambda r: (-r["value"], r["height"], r["outpoint"]))
    out += ["", "(a) unspent outputs of witness version 2 with a 32-byte program, by value: %d" % len(unspent)]
    for r in unspent:
        out.append("  value=%s height=%d coinbase=%s outpoint=%s:%d address=%s" % (
            fec(r["value"]), r["height"], "yes" if r["coinbase"] else "no", r["outpoint"][0], r["outpoint"][1],
            address(r["script"], chain)))
    out += ["", "(a) spent outputs of witness version 2 with a 32-byte program: %d" % len(census.spent)]
    for r in census.spent:
        out.append("  height=%d value=%s coinbase=%s outpoint=%s:%d address=%s spent_height=%d spent_txid=%s "
                   "bip360=%s" % (r["height"], fec(r["value"]), "yes" if r["coinbase"] else "no", r["outpoint"][0],
                                  r["outpoint"][1], address(r["script"], chain), r["spent_height"], r["spent_txid"],
                                  r["bip360"]))
    out += ["", "(a) outputs of witness version 2 with another program length: %d" % len(census.other)]
    for r in sorted(census.other.values(), key=lambda r: (r["height"], r["outpoint"])):
        out.append("  height=%d length=%d value=%s outpoint=%s:%d address=%s spent_height=%s" % (
            r["height"], r["length"], fec(r["value"]), r["outpoint"][0], r["outpoint"][1],
            address(r["script"], chain), r.get("spent_height", "unspent")))

    out += ["", "signalling: blocks per window whose version has the top bits 001 and the bit set "
            "(windows of %d blocks; windows with none are left out)" % census.window]
    maxima = [0, 0, 0]
    for index in sorted(census.signals):
        counts = census.signals[index]
        maxima = [max(a, b) for a, b in zip(maxima, counts)]
        out.append("  window=%s bit6=%d bit7=%d bit8=%d" % (census.window_name(index), *counts))

    early = census.early
    miners = collections.Counter(e["miner"] for e in early)
    if not args.no_maturity:
        out += ["", "(b) spends of coinbase outputs %d to %d blocks after the block that created them: %d"
                % (MATURITY_OLD, MATURITY_NEW - 1, len(early)), "by miner tag of the coinbase:"]
        out += ["  count=%d miner=%r" % (n, m) for m, n in sorted(miners.items(), key=lambda x: (-x[1], x[0]))]
        out.append("by address of the coinbase output:")
        addresses = collections.Counter(e["address"] for e in early)
        out += ["  count=%d address=%s" % (n, a) for a, n in sorted(addresses.items(), key=lambda x: (-x[1], x[0]))]
        out.append("by window of the spending block:")
        windows = collections.Counter(e["height"] // census.window for e in early)
        out += ["  window=%s count=%d" % (census.window_name(w), n) for w, n in sorted(windows.items())]
        if len(early) <= 100:
            out.append("every spend:")
            out += [early_line(e) for e in early]
        else:
            out.append("the first 50 spends:")
            out += [early_line(e) for e in early[:50]]
            out.append("the last 50 spends:")
            out += [early_line(e) for e in early[-50:]]

    out += ["",
            "RESULT: v2_32_unspent=%d value=%s" % (len(unspent), fec(sum(r["value"] for r in unspent))),
            "RESULT: v2_32_created=%d spent=%d bip360_valid=%d bip360_invalid=%d" % (
                len(unspent) + len(census.spent), len(census.spent),
                sum(r["bip360"] == "valid" for r in census.spent), sum(r["bip360"] != "valid" for r in census.spent)),
            "RESULT: v2_other_created=%d" % len(census.other),
            "RESULT: signalling bit6_max=%d bit7_max=%d bit8_max=%d" % tuple(maxima)]
    if not args.no_maturity:
        out.append("RESULT: early_coinbase_spends=%d miners=%d" % (len(early), len(miners)))
    out.append("RESULT: range=%d-%d %s" % (first, last, "complete" if complete else "partial"))
    print("\n".join(out))
    report_rpc_time(rpc, started)


if __name__ == "__main__":
    run(main)
