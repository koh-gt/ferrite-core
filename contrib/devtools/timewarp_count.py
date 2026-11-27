#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Read-only count of the blocks that the block time rule of the timewarp deployment would refuse.

That rule tightens the block timestamps the difficulty calculation uses: once it is active, the
time of each block at height 24 or above must be at least 480 seconds after the time of the block
23 below it. For the blocks of the range (default: the last 100,000, never below height 24) the
script reports:
  - every block whose time is earlier than that, with its miner tag, the miner tag of the block
    23 below, the span between the two times and the lead of the block 23 below; the count, the
    count per 100,000 blocks, the smallest span, and counts by span and by miner;
  - the lead of each block: its time minus the median time of the 11 blocks after it, a lower
    bound for how far its time was ahead of the clock of the miners after it. The blocks whose
    lead is above 600, 1,200, 1,800 and 3,600 seconds are counted by miner, and the largest lead
    is named. The last 11 blocks of the range have no lead;
  - the mean interval between the blocks of the range.

The script calls only getblockchaininfo, getblockhash, getblockheader and getblock. It changes
nothing and writes no file: the report goes to standard output and progress to standard error. It
works on a pruned node, from headers; the miner tag of a block the node no longer stores is "?". It
connects only to the host and port of --rpcurl: no proxy is used and no redirect is followed.
Credentials are taken from --rpcuser/--rpcpassword, else (for a node on this computer only) from the
.cookie file or the rpcuser and rpcpassword lines of ferrite.conf in the data directory, else asked
for.

Usage (Python 3.8+, standard library only), on the computer that runs the node (server=1):
  python3 contrib/devtools/timewarp_count.py > timewarp.txt
  python3 contrib/devtools/timewarp_count.py --start 24          # the whole chain
  python  contrib\\devtools\\timewarp_count.py --testnet
The last lines start with "RESULT: " and summarise the report.
"""
import argparse
import base64
import collections
import decimal
import getpass
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

ALLOWED_METHODS = frozenset({"getblockchaininfo", "getblockhash", "getblock", "getblockheader"})

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


SPAN_DEPTH = 23
MIN_SPAN = 480
LOWEST_HEIGHT = SPAN_DEPTH + 1
LEAD_BLOCKS = 11
LEAD_LIMITS = (600, 1200, 1800, 3600)
HEADER_BATCH = 500
DEFAULT_BLOCKS = 100000
PRUNED = object()


def measure(times, medians):
    """The leads of a run of consecutive blocks. times[i] is the time of block i of the run and
    medians[i] its median time past (the median of its own time and the times of the 10 blocks
    before it, as getblockheader gives it). The lead of block i is times[i] - medians[i + 11], its
    time minus the median of the times of the 11 blocks after it; the last 11 blocks have none."""
    return [times[i] - medians[i + LEAD_BLOCKS] for i in range(len(times) - LEAD_BLOCKS)]


def main():
    parser = ArgumentParser(prog="timewarp_count.py", allow_abbrev=False, description=__doc__,
                            formatter_class=argparse.RawDescriptionHelpFormatter)
    common_arguments(parser)
    parser.add_argument("--batch", type=int, default=50, help="blocks per getblock request for the miner tags, "
                        "1 to 500 (default: %(default)s)")
    args = parser.parse_args()
    started = time.monotonic()
    rpc, info = connect(args)
    tip = info["blocks"]
    last = tip if args.end is None else args.end
    first = max(LOWEST_HEIGHT, last - DEFAULT_BLOCKS + 1) if args.start is None else args.start
    if not 0 <= first <= last <= tip:
        fail("--start and --end must give 0 <= start <= end <= %d (the tip)" % tip)
    notes, remarks = [], []
    if first < LOWEST_HEIGHT:
        remarks.append("the rule applies from height %d, so the range starts there" % LOWEST_HEIGHT)
        first = LOWEST_HEIGHT
    if last < first:
        fail("the range has no block at height %d or above" % LOWEST_HEIGHT)
    if first > LOWEST_HEIGHT or last < tip:
        notes.append("the range does not cover the chain from height %d (%d-%d)" % (LOWEST_HEIGHT, LOWEST_HEIGHT, tip))
    lowest = info["pruneheight"] if info["pruned"] else 0
    if lowest > first - SPAN_DEPTH:
        notes.append("the node is pruned: it has no block below %d. The counts are complete (they are read "
                     "from headers); the miner tag of a block below %d is \"?\"" % (lowest, lowest))
    if info["initialblockdownload"]:
        notes.append("the node is in initial block download")
    if tip < info["headers"]:
        notes.append("the node has %d blocks but %d headers" % (tip, info["headers"]))

    base = first - SPAN_DEPTH
    times, medians, hashes = [], [], []
    previous = None
    shown = time.monotonic()
    for low in range(base, last + 1, HEADER_BATCH):
        high = min(low + HEADER_BATCH - 1, last)
        block_hash_list = block_hashes(rpc, low, high)
        headers = rpc.results("getblockheader", [[h, True] for h in block_hash_list])
        for height, block_hash, header in zip(range(low, high + 1), block_hash_list, headers):
            check_link(header, height, block_hash, previous)
            want(is_int(header.get("mediantime"), 0, 2**32 - 1))
            times.append(header["time"])
            medians.append(header["mediantime"])
            hashes.append(block_hash)
            previous = block_hash
        if time.monotonic() - shown >= 10:
            shown = time.monotonic()
            print("  header %d of %d" % (high, last), file=sys.stderr)
    check_best_block(rpc, info, last, hashes[-1])

    leads = measure(times, medians)
    violations, smallest = [], None
    for h in range(first, last + 1):
        span = times[h - base] - times[h - SPAN_DEPTH - base]
        smallest = span if smallest is None else min(smallest, span)
        if span < MIN_SPAN:
            violations.append((h, span))
    ahead = [(h, leads[h - base]) for h in range(first, last - LEAD_BLOCKS + 1)]
    largest = max(ahead, key=lambda x: (x[1], -x[0])) if ahead else None

    wanted = set()
    for h, _ in violations:
        wanted.update((h, h - SPAN_DEPTH))
    wanted.update(h for h, lead in ahead if lead > LEAD_LIMITS[0])
    if largest is not None:
        wanted.add(largest[0])
    tags = {h: "?" for h in wanted if h < lowest}
    todo = sorted(h for h in wanted if h >= lowest)

    def pruned(error):
        return PRUNED if error["code"] == -1 and "pruned" in error["message"] else None

    for i in range(0, len(todo), args.batch):
        heights = todo[i:i + args.batch]
        blocks = rpc.results("getblock", [[hashes[h - base], 2] for h in heights], pruned)
        for h, block in zip(heights, blocks):
            if block is PRUNED:
                tags[h] = "?"
                continue
            want(isinstance(block, dict) and block.get("hash") == hashes[h - base] and block.get("height") == h)
            tags[h] = miner_tag(coinbase_hex(block))

    out = ["timewarp_count.py: blocks earlier than %d s after the block %d below them, and leads"
           % (MIN_SPAN, SPAN_DEPTH),
           "chain=%s blocks=%d headers=%d bestblockhash=%s initialblockdownload=%s pruned=%s" % (
               info["chain"], tip, info["headers"], info["bestblockhash"],
               "yes" if info["initialblockdownload"] else "no",
               "yes (pruneheight %d)" % lowest if info["pruned"] else "no"),
           "softforks:"] + softfork_lines(info)
    complete = not notes
    out.append("range=%d-%d %s" % (first, last, "complete" if complete else "partial"))
    out += ["note: " + n for n in notes + remarks]

    out += ["", "blocks whose time is earlier than %d s after the time of the block %d below: %d"
            % (MIN_SPAN, SPAN_DEPTH, len(violations))]
    for h, span in violations:
        out.append("  height=%d hash=%s span=%d lead_23_below=%d miner=%r miner_23_below=%r" % (
            h, hashes[h - base], span, leads[h - SPAN_DEPTH - base], tags[h], tags[h - SPAN_DEPTH]))
    bins = [sum(1 for _, s in violations if s < 240), sum(1 for _, s in violations if 240 <= s < 400),
            sum(1 for _, s in violations if 400 <= s)]
    out.append("by span: below_240=%d 240_to_399=%d 400_to_479=%d" % tuple(bins))
    out.append("by miner:")
    by_miner = collections.Counter(tags[h] for h, _ in violations)
    out += ["  count=%d miner=%r" % (n, m) for m, n in sorted(by_miner.items(), key=lambda x: (-x[1], x[0]))]

    over = []
    if ahead:
        out += ["", "leads (time minus the median time of the %d blocks after it) of blocks %d-%d:"
                % (LEAD_BLOCKS, first, last - LEAD_BLOCKS)]
    else:
        out += ["", "leads: the range has no block with %d blocks after it in the range" % LEAD_BLOCKS]
    for limit in LEAD_LIMITS:
        blocks = [h for h, lead in ahead if lead > limit]
        over.append(len(blocks))
        out.append("lead over %d s: %d" % (limit, len(blocks)))
        counts = collections.Counter(tags[h] for h in blocks)
        out += ["  count=%d miner=%r" % (n, m) for m, n in sorted(counts.items(), key=lambda x: (-x[1], x[0]))]
    if largest is not None:
        out.append("largest lead: lead=%d height=%d hash=%s miner=%r" % (
            largest[1], largest[0], hashes[largest[0] - base], tags[largest[0]]))

    mean = (times[last - base] - times[first - base]) // (last - first) if last > first else 0
    out += ["",
            "RESULT: floor_violations=%d per_100000=%.2f smallest_span=%d" % (
                len(violations), len(violations) * 100000 / (last - first + 1), smallest),
            "RESULT: ahead_over_600=%d ahead_over_1200=%d ahead_over_1800=%d ahead_over_3600=%d largest_lead=%s"
            % (tuple(over) + ("none" if largest is None else str(largest[1]),)),
            "RESULT: mean_interval=%d" % mean,
            "RESULT: range=%d-%d %s" % (first, last, "complete" if complete else "partial")]
    print("\n".join(out))
    report_rpc_time(rpc, started)


if __name__ == "__main__":
    run(main)
