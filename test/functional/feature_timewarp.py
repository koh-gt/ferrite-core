#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the block time rule of the timewarp deployment while it is active.

Both nodes run with -vbparams=timewarp:-1:0, so the rule applies from block 24: a block's
time must be at least 480 seconds after the time of the block 23 below it, as well as later
than the median time past. The limit of two hours into the future is unchanged. The nodes
use mock time, which the test moves forward between batches of blocks.

The test checks that
- the floor first applies to block 24, 480 seconds after block 1;
- getblocktemplate gives the earliest valid time as mintime and the later of it and the
  clock as curtime, also on the same tip while the clock moves, the node's own miner stamps
  blocks the same way, and the other node accepts every such block, also while the floor is
  ahead of the clock and in MWEB blocks;
- a block or header below the floor is refused through submitblock, submitheader, block
  proposals, and from a peer as a header, a compact block and a full block, with no penalty
  for the peer and nothing stored; a header built on it gets a getheaders reply, and a
  header that is too old still gets its sender disconnected;
- while no time is valid for the next block, which after a block stamped two hours ahead of
  the clock lasts until the clock is 480 seconds further, generatetoaddress,
  generatetodescriptor, generateblock and getblocktemplate fail, and then they stamp the
  earliest valid time.
"""

from test_framework.blocktools import NORMAL_GBT_REQUEST_PARAMS, create_block, create_coinbase
from test_framework.messages import (
    CBlock,
    CBlockHeader,
    FromHex,
    HeaderAndShortIDs,
    msg_block,
    msg_cmpctblock,
    msg_headers,
)
from test_framework.p2p import P2PInterface, p2p_lock
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

SPAN_DEPTH = 23
MIN_SPAN = 480
MAX_FUTURE = 7200
START_TIME = 1700000000
NO_VALID_TIME = "CreateNewBlock: TestBlockValidity failed: time-too-new, block timestamp too far in the future"


class TimewarpTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [["-vbparams=timewarp:-1:0", "-deprecatedrpc=banscore"]] * 2

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def set_clock(self, t):
        self.clock = t
        for n in self.nodes:
            n.setmocktime(t)

    def time_of(self, height):
        node = self.nodes[0]
        return node.getblockheader(node.getblockhash(height))['time']

    def floor(self, node=None):
        """The earliest valid time for the next block on node's chain, computed here."""
        node = node or self.nodes[0]
        tip = node.getblockheader(node.getbestblockhash())
        height = tip['height'] + 1
        earliest = tip['mediantime'] + 1
        if height > SPAN_DEPTH:
            earliest = max(earliest, node.getblockheader(node.getblockhash(height - SPAN_DEPTH))['time'] + MIN_SPAN)
        return earliest

    def template(self):
        return self.nodes[0].getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)

    def block(self, ntime, tmpl=None):
        """A block without transactions on node 0's tip with the given time."""
        tmpl = tmpl or self.template()
        block = create_block(int(tmpl['previousblockhash'], 16), create_coinbase(tmpl['height']), ntime, version=tmpl['version'])
        block.solve()
        return block

    def propose(self, block):
        return self.nodes[0].getblocktemplate({'data': block.serialize().hex(), 'mode': 'proposal', 'rules': ['segwit']})

    def check_template(self, propose=True):
        """mintime is the floor and curtime the later of it and the clock; the node accepts both, and refuses one
        second less than mintime."""
        tmpl = self.template()
        floor = self.floor()
        assert_equal(tmpl['mintime'], floor)
        assert_equal(tmpl['curtime'], max(floor, self.clock))
        if propose:
            assert_equal(self.propose(self.block(tmpl['curtime'], tmpl)), None)
            assert_equal(self.propose(self.block(tmpl['mintime'], tmpl)), None)
            mtp = self.nodes[0].getblockheader(tmpl['previousblockhash'])['mediantime']
            expected = "time-too-old" if floor == mtp + 1 else "time-timewarp-span"
            assert_equal(self.propose(self.block(tmpl['mintime'] - 1, tmpl)), expected)
        return tmpl

    def mine(self, count, step, check=True, propose=True):
        """Mine count blocks on node 0, moving the clock step seconds before each, and check that each block has the
        time the template gave."""
        node = self.nodes[0]
        for _ in range(count):
            self.set_clock(self.clock + step)
            tmpl = self.check_template(propose) if check else None
            block_hash = node.generatetoaddress(1, self.address)[0]
            if check:
                assert_equal(node.getblockheader(block_hash)['time'], tmpl['curtime'])
        self.sync_blocks()

    def run_test(self):
        node, other = self.nodes
        self.address = node.getnewaddress()
        self.set_clock(START_TIME)

        self.log.info("The floor first applies to block 24: 480 seconds after block 1")
        # Blocks 1 to 23 one second apart, so that the floor of block 24 is above the median time past.
        self.mine(23, 1)
        assert_equal(node.getblockchaininfo()['softforks']['timewarp']['active'], True)
        tmpl = self.check_template()
        assert_equal(tmpl['height'], 24)
        assert_equal(tmpl['mintime'], self.time_of(1) + MIN_SPAN)
        assert tmpl['mintime'] > node.getblockheader(node.getbestblockhash())['mediantime'] + 1
        self.mine(7, 60)

        self.log.info("With the clock standing still the floor binds: the miner stamps it, ahead of the clock")
        self.mine(40, 0)
        tip_time = node.getblockheader(node.getbestblockhash())['time']
        assert tip_time > self.clock, (tip_time, self.clock)
        assert_equal(other.getbestblockhash(), node.getbestblockhash())

        self.log.info("The clock moves on: blocks are stamped with the clock again")
        self.set_clock(tip_time + 60)
        # A template on the same tip, asked for again later, takes the clock again.
        first = self.template()
        self.set_clock(self.clock + 30)
        second = self.template()
        assert_equal(second['previousblockhash'], first['previousblockhash'])
        assert_equal((first['curtime'], second['curtime']), (self.clock - 30, self.clock))
        self.mine(30, 60)
        assert_equal(node.getblockheader(node.getbestblockhash())['time'], self.clock)

        self.log.info("A block below the floor is refused through the RPCs, with nothing stored")
        self.mine(30, 1)
        floor = self.floor()
        assert floor > node.getblockheader(node.getbestblockhash())['mediantime'] + 1
        tip = node.getbestblockhash()
        below = self.block(floor - 1)
        assert_equal(node.submitblock(below.serialize().hex()), "time-timewarp-span")
        assert_equal(self.propose(below), "time-timewarp-span")
        assert_raises_rpc_error(-25, "time-timewarp-span", node.submitheader, CBlockHeader(below).serialize().hex())
        assert_raises_rpc_error(-5, "Block not found", node.getblockheader, below.hash)
        assert_equal(node.getbestblockhash(), tip)
        # The future limit is unchanged.
        assert_raises_rpc_error(-25, "time-too-new", node.submitheader, CBlockHeader(self.block(self.clock + MAX_FUTURE + 1)).serialize().hex())
        assert_equal(node.submitheader(CBlockHeader(self.block(self.clock + MAX_FUTURE)).serialize().hex()), None)
        # A header received before its block.
        at_floor = self.block(floor)
        assert_equal(node.submitheader(CBlockHeader(at_floor).serialize().hex()), None)
        assert_equal(node.submitblock(at_floor.serialize().hex()), None)
        assert_equal(node.getbestblockhash(), at_floor.hash)
        self.sync_blocks()

        self.log.info("A peer that sends a block below the floor is not punished and stays connected")
        floor = self.floor()
        below = self.block(floor - 1)
        tip = node.getbestblockhash()
        peer = node.add_p2p_connection(P2PInterface())
        for message in [msg_headers([CBlockHeader(below)]), self.compact(below), msg_block(below)]:
            with node.assert_debug_log(expected_msgs=["time-timewarp-span"], unexpected_msgs=["Misbehaving"]):
                peer.send_and_ping(message)
            assert peer.is_connected
            assert_equal(node.getpeerinfo()[-1]['banscore'], 0)
            assert_equal(node.getbestblockhash(), tip)
            assert_raises_rpc_error(-5, "Block not found", node.getblockheader, below.hash)
        # A header on the refused one does not connect, and the node asks the peer for headers.
        child = create_block(below.sha256, create_coinbase(node.getblockcount() + 2), floor + 60)
        child.solve()
        with p2p_lock:
            peer.last_message.pop("getheaders", None)
        peer.send_message(msg_headers([CBlockHeader(child)]))
        peer.wait_for_getheaders()
        assert peer.is_connected
        assert_equal(node.getpeerinfo()[-1]['banscore'], 0)
        # A header that is too old (at the median time past) still gets its sender disconnected.
        too_old = self.block(node.getblockheader(tip)['mediantime'])
        bad_peer = node.add_p2p_connection(P2PInterface())
        with node.assert_debug_log(expected_msgs=["time-too-old"]):
            bad_peer.send_message(msg_headers([CBlockHeader(too_old)]))
            bad_peer.wait_for_disconnect()
        node.disconnect_p2ps()

        self.log.info("While no time is valid the miner fails, until the clock is 480 seconds further")
        # A block two hours ahead of the clock, then 22 blocks with the clock moving one second per block.
        c0 = self.clock + 1
        self.set_clock(c0)
        ahead = self.block(c0 + MAX_FUTURE)
        assert_equal(node.submitblock(ahead.serialize().hex()), None)
        self.mine(22, 1, propose=False)
        assert_equal(self.floor(), c0 + MAX_FUTURE + MIN_SPAN)
        self.set_clock(c0 + MIN_SPAN - 1)
        descriptor = node.getdescriptorinfo("addr(%s)" % self.address)['descriptor']
        assert_raises_rpc_error(-1, NO_VALID_TIME, node.generatetoaddress, 1, self.address)
        assert_raises_rpc_error(-1, NO_VALID_TIME, node.generatetodescriptor, 1, descriptor)
        assert_raises_rpc_error(-1, NO_VALID_TIME, node.generateblock, self.address, [])
        assert_raises_rpc_error(-1, NO_VALID_TIME, node.getblocktemplate, NORMAL_GBT_REQUEST_PARAMS)
        self.set_clock(c0 + MIN_SPAN)
        tmpl = self.check_template()
        assert_equal(tmpl['curtime'], c0 + MAX_FUTURE + MIN_SPAN)
        block_hash = node.generatetoaddress(1, self.address)[0]
        assert_equal(node.getblockheader(block_hash)['time'], c0 + MAX_FUTURE + MIN_SPAN)
        # generatetodescriptor and generateblock stamp the earliest valid time too, which is ahead of the clock.
        for generate in (lambda: node.generatetodescriptor(1, descriptor)[0], lambda: node.generateblock(self.address, [])['hash']):
            floor = self.floor()
            assert floor > self.clock
            assert_equal(node.getblockheader(generate())['time'], floor)
        self.sync_blocks()

        self.log.info("MWEB blocks follow the same rule")
        self.set_clock(self.time_of(node.getblockcount()) + 60)
        self.mine(431 - node.getblockcount(), 60, check=False)
        node.sendtoaddress(node.getnewaddress(address_type='mweb'), 10)
        self.mine(1, 60, propose=False)
        assert 'mweb' in node.getblock(node.getbestblockhash())
        # The floor binds in MWEB blocks with MWEB transactions, the clock moving one second per block.
        for _ in range(30):
            node.sendtoaddress(node.getnewaddress(address_type='mweb'), 1)
            self.mine(1, 1, propose=False)
        assert node.getblockheader(node.getbestblockhash())['time'] > self.clock
        assert_equal(other.getbestblockhash(), node.getbestblockhash())
        # A block that node 0 made, stamped one second below the floor and solved again, is refused by node 1.
        self.disconnect_nodes(0, 1)
        block_hash = node.generatetoaddress(1, self.address)[0]
        block = FromHex(CBlock(), node.getblock(block_hash, 0))
        assert_equal(block.nTime, self.floor(other))
        assert block.nTime > other.getblockheader(other.getbestblockhash())['mediantime'] + 1
        block.nTime -= 1
        block.solve()
        assert_equal(other.submitblock(block.serialize().hex()), "time-timewarp-span")
        assert_equal(other.submitblock(node.getblock(block_hash, 0)), None)
        assert_equal(other.getbestblockhash(), block_hash)
        self.connect_nodes(0, 1)

        self.log.info("The help of getblocktemplate describes mintime and curtime")
        help_text = node.help("getblocktemplate")
        assert "Once the timewarp rules are active, also at least 480 seconds after the time of the block 23 below the next block." in help_text
        assert "Once the timewarp rules are active, at least mintime, so it can be ahead of the node's clock." in help_text

    def compact(self, block):
        compact_block = HeaderAndShortIDs()
        compact_block.initialize_from_block(block)
        return msg_cmpctblock(compact_block.to_p2p())


if __name__ == '__main__':
    TimewarpTest().main()
