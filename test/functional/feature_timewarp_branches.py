#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the block time rule with several locked-in windows, on header-only branches, and for peers.

Node 0 signals the timewarp deployment from block 144 and keeps it locked in for three windows, so the rule
applies from block 720; node 1 keeps it locked in for one window (rule from block 432); on node 2 the rule is
always active. The test checks
- the heights before, at and after the activation height for both: a block one second below the floor is valid
  before it and refused from it on, the floor itself and one second above it are valid, and mintime follows;
- that a header is decided with the state and the blocks of its own branch: on a header-only branch from a tip
  that is not yet under the rule, on a header-only branch whose block 23 below differs from the active chain's,
  and on a header-only side branch on which the deployment locks in one window later;
- the scores of a peer that sends headers and blocks built on a header refused below the floor: unconnecting
  headers count as before, ten of them cost 20 points, a header that connects resets the count, a message of
  eight headers and an unrequested block cost 10 points each, and a compact block costs nothing;
- that a block 23 blocks after one stamped two hours ahead can be made exactly 480 seconds after the 22 blocks
  between were accepted, and not one second earlier.
"""

from test_framework.blocktools import NORMAL_GBT_REQUEST_PARAMS, create_block, create_coinbase
from test_framework.messages import CBlockHeader, HeaderAndShortIDs, msg_block, msg_cmpctblock, msg_headers
from test_framework.p2p import P2PInterface, p2p_lock
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

WINDOW = 144
START = 144
TIMEOUT = 99999999
SPAN_DEPTH = 23
MIN_SPAN = 480
MAX_FUTURE = 7200
START_TIME = 1700000000
NOSIG = 0x20000000
SIG = NOSIG | (1 << 8)
NO_VALID_TIME = "CreateNewBlock: TestBlockValidity failed: time-too-new, block timestamp too far in the future"


def median(times):
    return sorted(times)[len(times) // 2]


def hdr(block):
    return CBlockHeader(block).serialize().hex()


class TimewarpBranchesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        common = ["-vbparams=mweb:-2:0", "-deprecatedrpc=banscore"]
        self.extra_args = [
            common + ["-vbparams=timewarp:0:0:{}:{}:3".format(START, TIMEOUT)],
            common + ["-vbparams=timewarp:0:0:{}:{}".format(START, TIMEOUT)],
            common + ["-vbparams=timewarp:-1:0"],
        ]

    def setup_network(self):
        self.setup_nodes()

    def note(self, msg, *args):
        self.log.info(msg, *args)

    def set_clock(self, t):
        self.clock = t
        for n in self.nodes:
            n.setmocktime(t)

    def chain_times(self, node):
        return [node.getblockheader(node.getblockhash(h))['time'] for h in range(node.getblockcount() + 1)]

    def mine(self, node, count, step):
        addr = node.get_deterministic_priv_key().address
        for _ in range(count):
            self.set_clock(self.clock + step)
            node.generatetoaddress(1, addr)

    @staticmethod
    def make(prev_hash, height, t, version=NOSIG):
        prev = prev_hash if isinstance(prev_hash, int) else int(prev_hash, 16)
        block = create_block(prev, create_coinbase(height), t, version=version)
        block.solve()
        return block

    def propose(self, node, block):
        return node.getblocktemplate({'data': block.serialize().hex(), 'mode': 'proposal', 'rules': ['segwit']})

    def headers_chain(self, node, prev_hash, times, first_height, count, version, stamp, expect=None):
        """Submit headers on prev_hash. times[h] are the times up to prev; stamp(height, times) gives the time.
        expect(height) -> None for accepted or the reject reason; a refused header ends the chain."""
        prev = int(prev_hash, 16)
        hashes = {}
        for height in range(first_height, first_height + count):
            t = stamp(height, times)
            v = version(height) if callable(version) else version
            block = self.make(prev, height, t, v)
            reason = expect(height, t, times) if expect else None
            if reason is None:
                assert_equal(node.submitheader(hdr(block)), None)
            else:
                assert_raises_rpc_error(-25, reason, node.submitheader, hdr(block))
                return hashes, times
            times = times + [t]
            hashes[height] = block.hash
            prev = block.sha256
        return hashes, times

    def boundary_run(self, node, A):
        """Mine to A-2 (the last 30 blocks one second apart), then check A-1, A and A+1 with blocks and headers at
        L-1, L and L+1."""
        self.mine(node, A - 32 - node.getblockcount(), 60)
        self.mine(node, A - 2 - node.getblockcount(), 1)
        info = node.getblockchaininfo()['softforks']['timewarp']['bip8']
        assert_equal(info['status'], 'locked_in')
        assert_equal(info['activation_height'], A)
        for height in (A - 1, A, A + 1):
            times = self.chain_times(node)
            assert_equal(len(times), height)
            active = height >= A
            mtp = median(times[-11:])
            would = times[height - SPAN_DEPTH] + MIN_SPAN
            assert would > mtp + 1, (height, would, mtp)
            L = would if active else mtp + 1
            tmpl = node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
            assert_equal(tmpl['mintime'], L)
            assert_equal(tmpl['curtime'], max(L, self.clock))
            tip = node.getbestblockhash()
            if active:
                assert_equal(self.propose(node, self.make(tip, height, L - 1)), "time-timewarp-span")
                assert_raises_rpc_error(-25, "time-timewarp-span", node.submitheader, hdr(self.make(tip, height, L - 1)))
                assert_equal(node.submitblock(self.make(tip, height, L - 1).serialize().hex()), "time-timewarp-span")
                assert_raises_rpc_error(-5, "Block not found", node.getblockheader, self.make(tip, height, L - 1).hash)
            else:
                assert_equal(self.propose(node, self.make(tip, height, would - 1)), None)
                assert_equal(node.submitheader(hdr(self.make(tip, height, would - 1))), None)
            assert_equal(self.propose(node, self.make(tip, height, L)), None)
            assert_equal(node.submitheader(hdr(self.make(tip, height, L))), None)
            assert_equal(node.submitheader(hdr(self.make(tip, height, L + 1))), None)
            assert_raises_rpc_error(-25, "time-too-old", node.submitheader, hdr(self.make(tip, height, mtp)))
            # Extend the chain: A-1 at the median plus one (below what would be the floor), A at L, A+1 at L+1.
            t = {A - 1: mtp + 1, A: L, A + 1: L + 1}[height]
            block = self.make(tip, height, t)
            assert_equal(node.submitblock(block.serialize().hex()), None)
            assert_equal(node.getbestblockhash(), block.hash)
            self.note("  height %d (A%+d): mintime %d (median+1 %d, floor %d), checks pass", height, height - A, L, mtp + 1, would)
        info = node.getblockchaininfo()['softforks']['timewarp']
        assert_equal(info['bip8']['status'], 'active')
        assert_equal(info['height'], A)

    def run_test(self):
        n0, n1, n2 = self.nodes
        self.set_clock(START_TIME)

        self.note("Delay 1 (node 1): A = 432; A-1, A, A+1")
        self.boundary_run(n1, 2 * WINDOW + WINDOW)
        i = 1
        self.restart_node(i, extra_args=self.extra_args[i] + ["-mocktime={}".format(self.clock)])
        times = self.chain_times(n1)
        L = max(median(times[-11:]) + 1, times[len(times) - SPAN_DEPTH] + MIN_SPAN)
        assert_equal(n1.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)['mintime'], L)
        assert_raises_rpc_error(-25, "time-timewarp-span", n1.submitheader, hdr(self.make(n1.getbestblockhash(), len(times), L - 1)))

        self.note("Delay 3 (node 0): mine to 600 (locked in since 288, activation 720)")
        self.mine(n0, 600, 60)
        info = n0.getblockchaininfo()['softforks']['timewarp']['bip8']
        assert_equal((info['status'], info['since'], info['activation_height']), ('locked_in', 288, 720))
        A = 720

        self.note("Header-only branch from the tip at 600 across 720: the state comes from the branch, not the tip")
        times0 = self.chain_times(n0)

        def stamp_b1(height, times):
            if height < 690:
                return times[-1] + 60
            if height < A - 1:
                return times[-1] + 1
            if height == A - 1:
                return times[height - SPAN_DEPTH] + MIN_SPAN - 1  # below what would be the floor, valid at A-1
            return max(median(times[-11:]) + 1, times[height - SPAN_DEPTH] + MIN_SPAN)  # L

        hashes, times_b1 = self.headers_chain(n0, n0.getbestblockhash(), times0, 601, A - 601, NOSIG, stamp_b1)
        assert_equal(n0.getblockheader(hashes[A - 1])['height'], A - 1)
        # The tip (600) is locked in: the block after it is not under the rule.
        assert_equal(n0.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)['mintime'], median(times0[-11:]) + 1)
        for height in (A, A + 1):
            L = max(median(times_b1[-11:]) + 1, times_b1[height - SPAN_DEPTH] + MIN_SPAN)
            assert L > median(times_b1[-11:]) + 1
            prev = hashes[height - 1]
            assert_raises_rpc_error(-25, "time-timewarp-span", n0.submitheader, hdr(self.make(prev, height, L - 1)))
            at_floor = self.make(prev, height, L)
            assert_equal(n0.submitheader(hdr(at_floor)), None)
            hashes[height] = at_floor.hash
            times_b1 = times_b1 + [L]
        self.note("  header-only branch: A-1 below the floor stored, A and A+1 at L-1 refused, at L stored (tip at 600)")

        self.note("Delay 3 (node 0): A-1, A, A+1 with blocks")
        self.boundary_run(n0, A)

        self.note("Header-only branch from block 700: the block 23 below is the branch's own, not the active chain's")
        times0 = self.chain_times(n0)
        fork = 700
        times_fork = times0[:fork + 1]

        def stamp_fork(height, times):
            earliest = max(median(times[-11:]) + 1, times[height - SPAN_DEPTH] + MIN_SPAN)
            if height == fork + 1:
                return times0[fork + 1] + 300  # later than the active chain's block 701
            return earliest

        fork_hashes, times_fork = self.headers_chain(n0, n0.getblockhash(fork), times_fork, fork + 1, 23, NOSIG, stamp_fork)
        height = fork + 24
        L = max(median(times_fork[-11:]) + 1, times_fork[height - SPAN_DEPTH] + MIN_SPAN)
        floor_from_active_chain = max(median(times_fork[-11:]) + 1, times0[height - SPAN_DEPTH] + MIN_SPAN)
        assert L > floor_from_active_chain and L > median(times_fork[-11:]) + 1, (L, floor_from_active_chain)
        assert_raises_rpc_error(-25, "time-timewarp-span", n0.submitheader, hdr(self.make(fork_hashes[height - 1], height, L - 1)))
        assert_equal(n0.submitheader(hdr(self.make(fork_hashes[height - 1], height, L))), None)
        self.note("  at %d the floor is %d s above what the active chain's block %d would give; L-1 refused, L stored", height, L - floor_from_active_chain, height - SPAN_DEPTH)

        self.note("Header-only side branch from 143 that locks in one window later (active from 864)")
        times0 = self.chain_times(n0)
        LATE = A + WINDOW

        def version_late(height):
            return NOSIG if height < START + WINDOW else SIG

        def stamp_late(height, times):
            earliest = median(times[-11:]) + 1
            if height >= LATE:
                earliest = max(earliest, times[height - SPAN_DEPTH] + MIN_SPAN)
            if 700 <= height < 790 or 835 <= height < LATE:
                return earliest if height in (A, A + 1, A + 50) else max(earliest, times[-1] + 1)
            return max(earliest, times[-1] + 30)

        side, times_side = self.headers_chain(n0, n0.getblockhash(START - 1), times0[:START], START, LATE - START, version_late, stamp_late)
        # On this branch the floor does not apply between 720 and 863: headers below it were stored.
        for height in (A, A + 1, A + 50):
            would = times_side[height - SPAN_DEPTH] + MIN_SPAN
            assert times_side[height] < would, (height, times_side[height], would)
            assert_equal(n0.getblockheader(side[height])['height'], height)
        assert n0.getblockchaininfo()['softforks']['timewarp']['active']
        # At 864 it applies on this branch.
        L = max(median(times_side[-11:]) + 1, times_side[LATE - SPAN_DEPTH] + MIN_SPAN)
        assert L > median(times_side[-11:]) + 1
        assert_raises_rpc_error(-25, "time-timewarp-span", n0.submitheader, hdr(self.make(side[LATE - 1], LATE, L - 1, SIG)))
        assert_equal(n0.submitheader(hdr(self.make(side[LATE - 1], LATE, L, SIG))), None)
        self.note("  side branch: 720, 721, 770 below their would-be floor stored; 864 at L-1 refused, at L stored")

        self.note("Peers (node 2, always active): children of a refused header")
        self.mine(n2, 40, 60)
        self.mine(n2, 30, 1)
        times2 = self.chain_times(n2)
        height = len(times2)
        L = max(median(times2[-11:]) + 1, times2[height - SPAN_DEPTH] + MIN_SPAN)
        assert L > median(times2[-11:]) + 1
        tip = n2.getbestblockhash()
        refused = self.make(tip, height, L - 1)
        peer = n2.add_p2p_connection(P2PInterface())

        def score():
            peers = n2.getpeerinfo()
            assert_equal(len(peers), 1)
            return peers[0]['banscore']

        with n2.assert_debug_log(expected_msgs=["time-timewarp-span"], unexpected_msgs=["Misbehaving"]):
            peer.send_and_ping(msg_headers([CBlockHeader(refused)]))
        assert_equal(score(), 0)
        # Descendants of the refused header, one header per message.
        chain = []
        prev = refused
        for k in range(1, 21):
            child = self.make(prev.sha256, height + k, L + 60 * k)
            chain.append(child)
            prev = child
        counts = []
        for k in range(1, 11):
            with n2.assert_debug_log(expected_msgs=["nUnconnectingHeaders={}".format(k)]):
                peer.send_and_ping(msg_headers([CBlockHeader(chain[k - 1])]))
            counts.append(score())
        self.note("  scores after unconnecting announcements 1..10: %s", counts)
        assert_equal(counts, [0] * 9 + [20])
        # A header that connects resets the counter.
        valid = self.make(tip, height, L)
        with n2.assert_debug_log(expected_msgs=["resetting nUnconnectingHeaders (10 -> 0)"]):
            peer.send_and_ping(msg_headers([CBlockHeader(valid)]))
        counts = []
        for k in range(11, 21):
            peer.send_and_ping(msg_headers([CBlockHeader(chain[k - 1])]))
            counts.append(score())
        self.note("  scores after unconnecting announcements 11..20 (after a reset): %s", counts)
        assert_equal(counts, [20] * 9 + [40])
        # Eight headers whose first parent is the refused one: missing prev, 10 points.
        eight = []
        prev = refused
        for k in range(1, 9):
            child = self.make(prev.sha256, height + k, L + 61 * k)
            eight.append(child)
            prev = child
        with n2.assert_debug_log(expected_msgs=["(40 -> 50): invalid header received"]):
            peer.send_and_ping(msg_headers([CBlockHeader(b) for b in eight]))
        assert_equal(score(), 50)
        # An unsolicited block on the refused one: missing prev, 10 points.
        orphan = self.make(refused.sha256, height + 1, L + 59)
        with n2.assert_debug_log(expected_msgs=["(50 -> 60)"]):
            peer.send_and_ping(msg_block(orphan))
        assert_equal(score(), 60)
        # A compact block on the refused one: a getheaders, no points.
        cmpct_child = self.make(refused.sha256, height + 1, L + 58)
        compact = HeaderAndShortIDs()
        compact.initialize_from_block(cmpct_child)
        with p2p_lock:
            peer.last_message.pop("getheaders", None)
        with n2.assert_debug_log(expected_msgs=[], unexpected_msgs=["Misbehaving"]):
            peer.send_and_ping(msg_cmpctblock(compact.to_p2p()))
        peer.wait_for_getheaders()
        assert_equal(score(), 60)
        assert peer.is_connected
        self.note("  8-header announcement +10, unsolicited block +10, compact block +0 (total 60, still connected)")
        # The refused header itself, offered again, is refused again without points.
        with n2.assert_debug_log(expected_msgs=["time-timewarp-span"], unexpected_msgs=["Misbehaving"]):
            peer.send_and_ping(msg_headers([CBlockHeader(refused)]))
        assert_equal(score(), 60)
        n2.disconnect_p2ps()

        self.note("Order: too old and below the floor is punished; below the floor and too new reports the floor")
        mtp = median(self.chain_times(n2)[-11:])
        bad_peer = n2.add_p2p_connection(P2PInterface())
        with n2.assert_debug_log(expected_msgs=["time-too-old", "Misbehaving"]):
            bad_peer.send_message(msg_headers([CBlockHeader(self.make(tip, height, mtp))]))
            bad_peer.wait_for_disconnect()
        n2.disconnect_p2ps()
        saved = self.clock
        self.set_clock(L - MAX_FUTURE - 2)
        assert_raises_rpc_error(-25, "time-timewarp-span", n2.submitheader, hdr(self.make(tip, height, L - 1)))
        assert_raises_rpc_error(-25, "time-too-new", n2.submitheader, hdr(self.make(tip, height, L + 1)))
        self.set_clock(saved)

        self.note("Liveness: the clock stands still while 23 blocks are accepted: the wait is exactly 480 s")
        assert_equal(n2.submitblock(valid.serialize().hex()), None)
        c0 = self.clock + 1
        self.set_clock(c0)
        times2 = self.chain_times(n2)
        ahead = self.make(n2.getbestblockhash(), len(times2), c0 + MAX_FUTURE)
        assert_equal(n2.submitblock(ahead.serialize().hex()), None)
        addr = n2.get_deterministic_priv_key().address
        for _ in range(22):
            n2.generatetoaddress(1, addr)  # clock stays at c0
        times2 = self.chain_times(n2)
        L = max(median(times2[-11:]) + 1, times2[len(times2) - SPAN_DEPTH] + MIN_SPAN)
        assert_equal(L, c0 + MAX_FUTURE + MIN_SPAN)
        for t in (c0, c0 + 1, c0 + MIN_SPAN - 1):
            self.set_clock(t)
            assert_raises_rpc_error(-1, NO_VALID_TIME, n2.getblocktemplate, NORMAL_GBT_REQUEST_PARAMS)
            assert_raises_rpc_error(-1, NO_VALID_TIME, n2.generatetoaddress, 1, addr)
        self.set_clock(c0 + MIN_SPAN)
        tmpl = n2.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
        assert_equal((tmpl['mintime'], tmpl['curtime']), (L, L))
        self.note("  no template at clock c0 .. c0+479 (%s), a template at c0+480 with curtime = mintime = c0+7680", NO_VALID_TIME)


if __name__ == '__main__':
    TimewarpBranchesTest().main()
