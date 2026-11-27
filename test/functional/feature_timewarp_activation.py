#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the signalled activation of the block time rule (the timewarp deployment).

Once the deployment is active for a block at height 24 or more, the block's time must be at
least 480 seconds after the time of the block 23 below it. Node 0 mines with the default block
version, so it signals every started deployment. Node 1 mines with -blockversion=0x20000000,
like a pool that does not signal. Both nodes use -vbparams to let the deployment signal from
START_HEIGHT (regtest windows are 144 blocks, 108 of them lock in), and mock time. MWEB is
switched off, so that the test can build plain blocks.

The test checks what getblockchaininfo and getblocktemplate show in each state of the
deployment, and that the rule applies to exactly the blocks from the activation height on: a
block below the floor is valid at the height before and refused from it on, while the limit of
two hours into the future is the same on both sides, and mintime follows. It restarts node 0
at each of these heights, sends it headers that cross the activation height, and reorganises
it onto a branch that leaves the activation height as it is, onto a branch on which the
deployment activates one window later, and back.
"""

from test_framework.blocktools import NORMAL_GBT_REQUEST_PARAMS, create_block, create_coinbase
from test_framework.messages import CBlockHeader, msg_headers
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

WINDOW = 144
THRESHOLD = 108
START_HEIGHT = WINDOW
TIMEOUT_HEIGHT = 99999999
# Every block of the first window signals, the second window is locked in.
ACTIVATION_HEIGHT = START_HEIGHT + 2 * WINDOW
# On the branch whose first window does not lock in, one window later.
LATE_ACTIVATION_HEIGHT = ACTIVATION_HEIGHT + WINDOW
DEPLOYMENT_BIT = 8
NOT_SIGNALLING_VERSION = 0x20000000
SIGNALLING_VERSION = NOT_SIGNALLING_VERSION | (1 << DEPLOYMENT_BIT)
SPAN_DEPTH = 23
MIN_SPAN = 480
MAX_FUTURE = 7200
START_TIME = 1700000000


def median(times):
    return sorted(times)[len(times) // 2]


class TimewarpActivationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        common = ["-vbparams=mweb:-2:0", "-vbparams=timewarp:0:0:{}:{}".format(START_HEIGHT, TIMEOUT_HEIGHT)]
        self.extra_args = [common, common + ["-blockversion={}".format(NOT_SIGNALLING_VERSION)]]

    def set_clock(self, t):
        self.clock = t
        for n in self.nodes:
            n.setmocktime(t)

    def deployment(self, node=None):
        return (node or self.nodes[0]).getblockchaininfo()['softforks']['timewarp']

    def template(self, node=None):
        return (node or self.nodes[0]).getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)

    def chain_times(self, node=None):
        node = node or self.nodes[0]
        return [node.getblockheader(node.getblockhash(h))['time'] for h in range(node.getblockcount() + 1)]

    @staticmethod
    def earliest(times, height, activation_height):
        """The earliest valid time of the block at this height on a chain with these earlier times."""
        earliest = median(times[height - 11:height]) + 1
        if height >= activation_height and height > SPAN_DEPTH:
            earliest = max(earliest, times[height - SPAN_DEPTH] + MIN_SPAN)
        return earliest

    def mine(self, count, step, node=None):
        node = node or self.nodes[0]
        for _ in range(count):
            self.set_clock(self.clock + step)
            node.generate(1)
        self.sync_blocks()

    def restart(self):
        """Restart node 0 with the clock as it is, and reconnect it."""
        self.restart_node(0, extra_args=self.extra_args[0] + ["-mocktime={}".format(self.clock)])
        self.connect_nodes(0, 1)

    def times_to(self, block_hash):
        """The times of the blocks from the genesis block to this one (on any branch node 0 knows)."""
        node = self.nodes[0]
        times = []
        while True:
            header = node.getblockheader(block_hash)
            times.append(header['time'])
            if header['height'] == 0:
                return list(reversed(times))
            block_hash = header['previousblockhash']

    def build(self, parent_hash, count, activation_height, version=SIGNALLING_VERSION, stamp=None):
        """Blocks on parent_hash, on a branch whose rule applies from activation_height. stamp(height, earliest valid
        time, median time past plus one) gives a block's time; by default, or when it returns None, it is 60 seconds
        after the parent's, or the earliest valid time if that is later. The clock is moved so that every block is in
        the past."""
        times = self.times_to(parent_hash)
        prev = int(parent_hash, 16)
        blocks = []
        for height in range(len(times), len(times) + count):
            earliest = self.earliest(times, height, activation_height)
            t = stamp(height, earliest, median(times[-11:]) + 1) if stamp else None
            if t is None:
                t = max(earliest, times[-1] + 60)
            v = version(height) if callable(version) else version
            block = create_block(prev, create_coinbase(height), t, version=v)
            block.solve()
            blocks.append(block)
            times.append(t)
            prev = block.sha256
        self.set_clock(max(self.clock, max(times)))
        return blocks

    def submit(self, blocks, node=None):
        node = node or self.nodes[0]
        for block in blocks:
            # A valid block that does not (yet) make its branch the active chain is "inconclusive".
            assert node.submitblock(block.serialize().hex()) in (None, "inconclusive")
            node.getblockheader(block.hash)

    def header_hex(self, time, prev_height=None):
        """A header on node 0's tip (or on the block at prev_height) with this time."""
        node = self.nodes[0]
        height = node.getblockcount() if prev_height is None else prev_height
        block = create_block(int(node.getblockhash(height), 16), create_coinbase(height + 1), time, version=SIGNALLING_VERSION)
        block.solve()
        return CBlockHeader(block).serialize().hex()

    def check_rule(self, active):
        """Check the next block on node 0's tip: mintime, curtime, and which headers are valid."""
        node = self.nodes[0]
        times = self.chain_times()
        height = len(times)
        mtp = median(times[-11:])
        floor = times[height - SPAN_DEPTH] + MIN_SPAN
        # The floor lies above the median here, so the two states differ.
        assert floor > mtp + 1, (floor, mtp)
        earliest = floor if active else mtp + 1
        tmpl = self.template()
        assert_equal(tmpl['mintime'], earliest)
        assert_equal(tmpl['curtime'], max(earliest, self.clock))
        if active:
            assert_raises_rpc_error(-25, "time-timewarp-span", node.submitheader, self.header_hex(floor - 1))
        else:
            assert_equal(node.submitheader(self.header_hex(floor - 1)), None)
            assert_equal(node.submitheader(self.header_hex(mtp + 1)), None)
        assert_equal(node.submitheader(self.header_hex(floor)), None)
        # The future limit is the same on both sides.
        assert_equal(node.submitheader(self.header_hex(self.clock + MAX_FUTURE)), None)
        assert_raises_rpc_error(-25, "time-too-new", node.submitheader, self.header_hex(self.clock + MAX_FUTURE + 1))
        assert_raises_rpc_error(-25, "time-too-old", node.submitheader, self.header_hex(mtp))
        return tmpl

    def run_test(self):
        node, other_pool = self.nodes
        self.set_clock(START_TIME)

        self.log.info("Defined: no bit in templates, mintime is the median time past plus one")
        self.mine(START_HEIGHT - 2, 60)
        assert_equal(self.deployment(), {
            'type': 'bip8',
            'bip8': {'status': 'defined', 'start_height': START_HEIGHT, 'timeout_height': TIMEOUT_HEIGHT, 'since': 0},
            'active': False,
        })
        tmpl = self.template()
        assert 'timewarp' not in tmpl['vbavailable']
        assert 'timewarp' not in tmpl['rules']
        assert_equal(tmpl['version'] & (1 << DEPLOYMENT_BIT), 0)
        assert_equal(tmpl['mintime'], node.getblockheader(node.getbestblockhash())['mediantime'] + 1)

        self.log.info("Signalling starts at the start height, and templates for segwit-only callers set bit 8")
        self.mine(1, 60)
        assert_equal(self.deployment(), {
            'type': 'bip8',
            'bip8': {
                'status': 'started',
                'bit': DEPLOYMENT_BIT,
                'start_height': START_HEIGHT,
                'timeout_height': TIMEOUT_HEIGHT,
                'since': START_HEIGHT,
                'statistics': {'period': WINDOW, 'threshold': THRESHOLD, 'elapsed': 0, 'count': 0, 'possible': True},
            },
            'active': False,
        })
        tmpl = self.template()
        assert_equal(tmpl['vbavailable']['timewarp'], DEPLOYMENT_BIT)
        assert 'timewarp' not in tmpl['rules']
        assert tmpl['version'] & (1 << DEPLOYMENT_BIT)
        # The pool that does not signal (its version is fixed by -blockversion).
        assert_equal(self.template(other_pool)['version'], NOT_SIGNALLING_VERSION)
        self.fork_height = node.getblockcount()

        self.log.info("A window in which every block signals locks in; the rule does not apply yet")
        self.mine(WINDOW, 60)
        assert_equal(self.deployment(), {
            'type': 'bip8',
            'bip8': {'status': 'locked_in', 'start_height': START_HEIGHT, 'timeout_height': TIMEOUT_HEIGHT, 'since': START_HEIGHT + WINDOW,
                     'activation_height': START_HEIGHT + 2 * WINDOW},
            'active': False,
        })
        tmpl = self.template()
        assert_equal(tmpl['vbavailable']['timewarp'], DEPLOYMENT_BIT)
        assert tmpl['version'] & (1 << DEPLOYMENT_BIT)

        self.log.info("Below the activation height a block below the floor is valid")
        # Blocks a minute apart, then blocks one second apart, so that the floor lies above the median.
        self.mine(ACTIVATION_HEIGHT - 32 - node.getblockcount(), 60)
        self.mine(ACTIVATION_HEIGHT - 2 - node.getblockcount(), 1)
        assert_equal(node.getblockcount(), ACTIVATION_HEIGHT - 2)
        tmpl = self.check_rule(active=False)
        assert_equal(self.deployment()['bip8']['status'], 'locked_in')
        self.restart()
        self.check_rule(active=False)
        # Block A-1 at the median time past plus one, below what would be the floor.
        block = create_block(int(tmpl['previousblockhash'], 16), create_coinbase(ACTIVATION_HEIGHT - 1), tmpl['mintime'], version=SIGNALLING_VERSION)
        block.solve()
        assert_equal(node.submitblock(block.serialize().hex()), None)
        self.sync_blocks()
        assert_equal(other_pool.getbestblockhash(), block.hash)

        self.log.info("The rule applies from the first block after the locked-in window")
        assert_equal(self.deployment(), {
            'type': 'bip8',
            'bip8': {'status': 'active', 'start_height': START_HEIGHT, 'timeout_height': TIMEOUT_HEIGHT, 'since': ACTIVATION_HEIGHT},
            'height': ACTIVATION_HEIGHT,
            'active': True,
        })
        tmpl = self.check_rule(active=True)
        assert 'timewarp' in tmpl['rules']
        assert 'timewarp' not in tmpl['vbavailable']
        assert_equal(tmpl['version'] & (1 << DEPLOYMENT_BIT), 0)
        below = create_block(int(tmpl['previousblockhash'], 16), create_coinbase(ACTIVATION_HEIGHT), tmpl['mintime'] - 1, version=NOT_SIGNALLING_VERSION)
        below.solve()
        assert_equal(node.submitblock(below.serialize().hex()), "time-timewarp-span")
        assert_equal(node.getblocktemplate({'data': below.serialize().hex(), 'mode': 'proposal', 'rules': ['segwit']}), "time-timewarp-span")
        self.restart()
        self.check_rule(active=True)
        assert_equal(node.submitblock(below.serialize().hex()), "time-timewarp-span")
        # Block A from the pool that never signalled: stamped at the floor, ahead of the clock, and valid.
        assert tmpl['mintime'] > self.clock
        block_hash = other_pool.generate(1)[0]
        assert_equal(other_pool.getblockheader(block_hash)['time'], tmpl['mintime'])
        self.sync_blocks()
        assert_equal(node.getbestblockhash(), block_hash)

        self.log.info("The block after the activation height")
        self.check_rule(active=True)
        self.restart()
        tmpl = self.check_rule(active=True)
        block_hash = node.generate(1)[0]
        assert_equal(node.getblockheader(block_hash)['time'], tmpl['curtime'])
        self.sync_blocks()

        self.log.info("Headers across the activation height: the first one below the floor from it on is refused")
        # A branch from block A-3 whose blocks are all at the median time past plus one.
        branch = self.build(node.getblockhash(ACTIVATION_HEIGHT - 3), 4, ACTIVATION_HEIGHT, stamp=lambda height, earliest, mtp_plus_one: mtp_plus_one)
        peer = node.add_p2p_connection(P2PInterface())
        with node.assert_debug_log(expected_msgs=["time-timewarp-span"], unexpected_msgs=["Misbehaving"]):
            peer.send_and_ping(msg_headers([CBlockHeader(b) for b in branch]))
        assert peer.is_connected
        node.getblockheader(branch[0].hash)
        node.getblockheader(branch[1].hash)
        for b in branch[2:]:
            assert_raises_rpc_error(-5, "Block not found", node.getblockheader, b.hash)
        node.disconnect_p2ps()

        self.log.info("Reorganisation across the activation height, onto a branch that keeps the rule")
        tip = node.getblockcount()
        fork_hash = node.getblockhash(ACTIVATION_HEIGHT - 3)
        branch = self.build(fork_hash, tip - ACTIVATION_HEIGHT + 8, ACTIVATION_HEIGHT)
        # A branch with a block at the activation height one second below the floor is refused there.
        bad = self.build(fork_hash, 2, ACTIVATION_HEIGHT, stamp=lambda height, earliest, mtp_plus_one: earliest)
        self.submit(bad)
        bad = self.build(bad[-1].hash, 1, ACTIVATION_HEIGHT, stamp=lambda height, earliest, mtp_plus_one: earliest - 1)
        assert_equal(node.submitblock(bad[0].serialize().hex()), "time-timewarp-span")
        self.submit(branch)
        assert_equal(node.getbestblockhash(), branch[-1].hash)
        self.sync_blocks()
        self.restart()
        assert_equal(node.getbestblockhash(), branch[-1].hash)
        assert_equal(self.deployment()['height'], ACTIVATION_HEIGHT)

        self.log.info("Reorganisation onto a branch on which the rule applies one window later")
        late_tip = LATE_ACTIVATION_HEIGHT + 30

        def late_version(height):
            # The first window does not signal, the second does.
            return NOT_SIGNALLING_VERSION if height < START_HEIGHT + WINDOW else SIGNALLING_VERSION

        def late_stamp(height, earliest, mtp_plus_one):
            # Between the two activation heights, blocks at their earliest time, below the floor of the other branch.
            if ACTIVATION_HEIGHT <= height < ACTIVATION_HEIGHT + 60:
                return earliest
            return None

        late = self.build(node.getblockhash(self.fork_height), late_tip - self.fork_height, LATE_ACTIVATION_HEIGHT, version=late_version, stamp=late_stamp)
        late_times = [b.nTime for b in late]
        first = ACTIVATION_HEIGHT + SPAN_DEPTH + 10 - self.fork_height - 1
        assert late_times[first] < late_times[first - SPAN_DEPTH] + MIN_SPAN
        self.submit(late)
        assert_equal(node.getbestblockhash(), late[-1].hash)
        assert_equal(self.deployment(), {
            'type': 'bip8',
            'bip8': {'status': 'active', 'start_height': START_HEIGHT, 'timeout_height': TIMEOUT_HEIGHT, 'since': LATE_ACTIVATION_HEIGHT},
            'height': LATE_ACTIVATION_HEIGHT,
            'active': True,
        })
        self.sync_blocks()
        assert_equal(other_pool.getbestblockhash(), late[-1].hash)
        self.restart()
        assert_equal(node.getbestblockhash(), late[-1].hash)
        assert_equal(self.deployment()['height'], LATE_ACTIVATION_HEIGHT)
        # Blocks a minute apart and then a second apart, so that the floor lies above the median again.
        self.mine(30, 1)
        self.check_rule(active=True)

        self.log.info("Reorganisation back onto the branch on which the rule applies from the first activation height")
        main_height = node.getblockheader(branch[-1].hash)['height']
        extension = self.build(branch[-1].hash, node.getblockcount() + 5 - main_height, ACTIVATION_HEIGHT)
        self.submit(extension)
        assert_equal(node.getbestblockhash(), extension[-1].hash)
        assert_equal(self.deployment()['height'], ACTIVATION_HEIGHT)
        self.sync_blocks()
        self.restart()
        assert_equal(node.getbestblockhash(), extension[-1].hash)
        assert_equal(self.deployment()['height'], ACTIVATION_HEIGHT)
        self.mine(30, 1)
        self.check_rule(active=True)
        # The pool that never signalled still mines blocks the other node accepts.
        self.mine(5, 0, node=other_pool)


if __name__ == '__main__':
    TimewarpActivationTest().main()
