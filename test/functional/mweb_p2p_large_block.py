#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a block larger than 4 MB with its MWEB data relays between nodes.

node0 mines a block whose transactions, with their witnesses, nearly fill the
block weight limit, and whose peg-in transactions add MWEB data that takes
its serialized size over 4,000,000 bytes, the length limit for messages
other than blocks and compact blocks. Check that:

- node1, which has the block's parent but none of its transactions, gets it
  as a compact block; the transactions it lacks arrive in one blocktxn
  message, which stays within 4,000,000 bytes;
- node2, which is three blocks behind, downloads the block in full, as one
  block message over 4,000,000 bytes.

node0 connects to node1 and node2, so for them it is an inbound peer.
"""

from test_framework.fec_util import setup_mweb_chain
from test_framework.key import compute_xonly_pubkey, generate_privkey
from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxInWitness,
    CTxOut,
    MAX_PROTOCOL_MESSAGE_LENGTH,
    WITNESS_SCALE_FACTOR,
)
from test_framework.script import CScript, OP_DROP, OP_TRUE, taproot_construct
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than, hex_str_to_bytes

HEADER_SIZE = 24
NUM_LARGE_TXS = 10
# Each of these transactions stays below the standard weight of 400,000, and
# together with the peg-ins they stay within the block weight limit
PUSHES_PER_SCRIPT = 750
NUM_PEGINS = 50


class MWEBP2PLargeBlockTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        # Each peg-in spends confirmed coins, rather than the MWEB change of the one before
        self.extra_args = [['-spendzeroconfchange=0'], [], []]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes(0, 1)
        self.connect_nodes(0, 2)

    def bytes_from_node0(self, node):
        """Bytes per message type that node has received from node0."""
        peers = [peer for peer in node.getpeerinfo() if 'testnode0' in peer['subver']]
        assert_equal(len(peers), 1)
        return peers[0]['bytesrecv_per_msg']

    def create_large_transactions(self):
        """Create transactions whose script path spends carry large witnesses."""
        node0 = self.nodes[0]
        script = CScript([b'\x01' * 520, OP_DROP] * PUSHES_PER_SCRIPT + [OP_TRUE])
        internal_pubkey, _ = compute_xonly_pubkey(generate_privkey())
        taproot = taproot_construct(internal_pubkey, [("large", script)])
        leaf = taproot.leaves["large"]
        control_block = bytes([leaf.version + taproot.negflag]) + taproot.inner_pubkey + leaf.merklebranch

        funding = CTransaction()
        funding.vout = [CTxOut(COIN, taproot.scriptPubKey) for _ in range(NUM_LARGE_TXS)]
        funded = node0.fundrawtransaction(funding.serialize().hex(), {'changePosition': NUM_LARGE_TXS})
        signed = node0.signrawtransactionwithwallet(funded['hex'])
        funding_txid = int(node0.sendrawtransaction(signed['hex']), 16)
        node0.generate(1)

        dest = hex_str_to_bytes(node0.getaddressinfo(node0.getnewaddress())['scriptPubKey'])
        txids = []
        for n in range(NUM_LARGE_TXS):
            tx = CTransaction()
            tx.vin = [CTxIn(COutPoint(funding_txid, n))]
            tx.vout = [CTxOut(COIN - COIN // 100, dest)]
            tx.wit.vtxinwit = [CTxInWitness()]
            tx.wit.vtxinwit[0].scriptWitness.stack = [script, control_block]
            tx.rehash()
            weight = len(tx.serialize_without_witness()) * (WITNESS_SCALE_FACTOR - 1) + len(tx.serialize())
            assert_greater_than(400000, weight)
            txids.append(node0.sendrawtransaction(tx.serialize().hex()))
        return txids

    def run_test(self):
        node0, node1, node2 = self.nodes

        self.log.info("Activate MWEB and sync all nodes")
        setup_mweb_chain(node0)
        self.sync_all()
        self.disconnect_nodes(0, 1)
        self.disconnect_nodes(0, 2)

        self.log.info("Fill node0's mempool with large transactions and peg-in transactions")
        txids = self.create_large_transactions()
        for _ in range(NUM_PEGINS):
            txids.append(node0.sendtoaddress(node1.getnewaddress(address_type='mweb'), 1))
        assert_equal(sorted(node0.getrawmempool()), sorted(txids))

        self.log.info("Connect node1 at the tip, with an empty mempool")
        self.connect_nodes(0, 1)
        self.sync_blocks([node0, node1])
        assert_equal(node1.getrawmempool(), [])
        before = self.bytes_from_node0(node1)

        self.log.info("Mine the large block")
        with node1.assert_debug_log(expected_msgs=[], unexpected_msgs=['HEADER ERROR']):
            block_hash = node0.generate(1)[0]
            self.sync_blocks([node0, node1])
        block = node0.getblock(block_hash, 2)
        assert 'mweb' in block
        assert set(txids[:NUM_LARGE_TXS]) <= set(tx['txid'] for tx in block['tx'])
        assert_equal(node0.getrawmempool(), [])
        block_size = len(node0.getblock(block_hash, 0)) // 2
        assert_equal(block['size'], block_size)
        assert_greater_than(block_size, MAX_PROTOCOL_MESSAGE_LENGTH)
        self.log.info("The block is {} bytes, weight {}".format(block_size, block['weight']))

        self.log.info("node1 got it as a compact block, with the missing transactions in one blocktxn message")
        after = self.bytes_from_node0(node1)
        received = {msgtype: after.get(msgtype, 0) - before.get(msgtype, 0) for msgtype in ['block', 'cmpctblock', 'blocktxn']}
        self.log.info("Bytes node1 received for it: {}".format(received))
        assert_equal(received['block'], 0)
        assert_greater_than(received['cmpctblock'], 0)
        assert_greater_than(received['blocktxn'], NUM_LARGE_TXS * PUSHES_PER_SCRIPT * 520)
        assert_greater_than(HEADER_SIZE + MAX_PROTOCOL_MESSAGE_LENGTH + 1, received['blocktxn'])
        assert_equal(node1.getblock(block_hash, 0), node0.getblock(block_hash, 0))

        self.log.info("Mine one more block and connect node2, which downloads all three in full")
        node0.generate(1)
        self.sync_blocks([node0, node1])
        with node2.assert_debug_log(expected_msgs=[], unexpected_msgs=['HEADER ERROR']):
            self.connect_nodes(0, 2)
            self.sync_blocks()
        received = self.bytes_from_node0(node2)
        self.log.info("Bytes node2 received as block messages: {}".format(received['block']))
        assert_greater_than(received['block'], HEADER_SIZE + block_size)
        assert_equal(received.get('blocktxn', 0), 0)
        assert_equal(node2.getblock(block_hash, 0), node0.getblock(block_hash, 0))


if __name__ == '__main__':
    MWEBP2PLargeBlockTest().main()
