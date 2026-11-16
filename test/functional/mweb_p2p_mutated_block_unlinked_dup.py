#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that blocks waiting for a discarded MWEB parent are linked once.

When the MWEB data of a block does not connect, the node discards that data
and keeps the block's descendants, which then wait for the parent in the block
index. Each time the node tries a chain through that parent, it records the
blocks of that chain as waiting for it. A block must be recorded only once,
however many chains pass through it, so that it is linked once when the parent
arrives with its correct data.

Node 0 tries two chains through the same parent, with a sibling of the same
height recorded in between. The test then sends the parent with its correct
data and checks that node 0 moves to the chain with the most work, and later
to the sibling's chain. The nodes run with -checkblockindex, whose checks of
the block index must pass throughout.
"""

import copy

from test_framework.fec_util import setup_mweb_chain
from test_framework.messages import CBlock, CBlockHeader, FromHex, msg_block, msg_headers
from test_framework.p2p import P2PDataStore
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class MWEBP2PMutatedBlockUnlinkedDupTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.extra_args = [['-whitelist=noban@127.0.0.1', '-checkblockindex=1'] for _ in range(self.num_nodes)]
        self.supports_cli = False

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def get_block(self, node, block_hash):
        block = FromHex(CBlock(), node.getblock(block_hash, 0))
        block.rehash()
        assert_equal(block.hash, block_hash)
        return block

    def run_test(self):
        node0, node1 = self.nodes
        miner0 = node0.get_wallet_rpc(self.default_wallet_name)
        miner1 = node1.get_wallet_rpc(self.default_wallet_name)

        self.log.info("Set up the MWEB chain and two MWEB coins")
        setup_mweb_chain(node0)
        node0.createwallet(wallet_name="spender0")
        node0.createwallet(wallet_name="spender1")
        spenders = [node0.get_wallet_rpc("spender0"), node0.get_wallet_rpc("spender1")]
        for spender in spenders:
            miner0.sendtoaddress(spender.getnewaddress(address_type="mweb"), 5)
            node0.generatetoaddress(1, miner0.getnewaddress())

        self.log.info("Create two single-input MWEB spends")
        for spender in spenders:
            spender.sendtoaddress(spender.getnewaddress(address_type="mweb"), 2)
        self.sync_all()
        assert_equal(len(node1.getrawmempool()), 2)

        self.log.info("Mine the blocks on node 1 only")
        self.disconnect_nodes(0, 1)
        tip = node0.getbestblockhash()
        parent_hash, child_hash, grandchild_hash = node1.generatetoaddress(3, miner1.getnewaddress())
        node1.invalidateblock(child_hash)
        sibling_hash = node1.generatetoaddress(1, miner1.getnewaddress())[0]

        parent = self.get_block(node1, parent_hash)
        child = self.get_block(node1, child_hash)
        sibling = self.get_block(node1, sibling_hash)
        grandchild = self.get_block(node1, grandchild_hash)
        assert_equal(parent.hashPrevBlock, int(tip, 16))
        assert_equal(child.hashPrevBlock, parent.sha256)
        assert_equal(sibling.hashPrevBlock, parent.sha256)
        assert_equal(grandchild.hashPrevBlock, child.sha256)
        assert_equal(len(parent.mweb_block.body.inputs), 2)

        self.log.info("Change the parent's MWEB inputs without changing its block hash")
        mutated_parent = copy.deepcopy(parent)
        inputs = mutated_parent.mweb_block.body.inputs
        assert inputs[0].commitment != inputs[1].commitment
        inputs[0].commitment, inputs[1].commitment = inputs[1].commitment, inputs[0].commitment
        for mweb_input in inputs:
            mweb_input.rehash()
        mutated_parent.rehash()
        assert_equal(mutated_parent.sha256, parent.sha256)

        peer = node0.add_p2p_connection(P2PDataStore())
        peer.send_and_ping(msg_headers([CBlockHeader(b) for b in (parent, child, grandchild)]))
        peer.send_and_ping(msg_headers([CBlockHeader(sibling)]))

        self.log.info("Send the child and its sibling, then the parent with changed MWEB data")
        peer.send_and_ping(msg_block(child))
        peer.send_and_ping(msg_block(sibling))
        with node0.assert_debug_log(expected_msgs=["mweb-connect-failed"]):
            peer.send_and_ping(msg_block(mutated_parent))
        assert_equal(node0.getbestblockhash(), tip)

        self.log.info("Send the grandchild, so that node 0 tries the child's chain again")
        peer.send_and_ping(msg_block(grandchild))
        assert_equal(node0.getbestblockhash(), tip)

        self.log.info("Send the parent with its correct data and move to the grandchild")
        node0.add_p2p_connection(P2PDataStore()).send_and_ping(msg_block(parent))
        self.wait_until(lambda: node0.getbestblockhash() == grandchild_hash)
        assert_equal(node0.getblock(sibling_hash)["confirmations"], -1)

        self.log.info("Move to the sibling's chain once it has the most work")
        assert_equal(node1.getbestblockhash(), sibling_hash)
        sibling_tip = node1.generatetoaddress(2, miner1.getnewaddress())[-1]
        self.connect_nodes(0, 1)
        self.wait_until(lambda: node0.getbestblockhash() == sibling_tip)
        assert_equal(node0.getblockhash(node0.getblockcount() - 2), sibling_hash)


if __name__ == '__main__':
    MWEBP2PMutatedBlockUnlinkedDupTest().main()
