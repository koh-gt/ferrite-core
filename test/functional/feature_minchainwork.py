#!/usr/bin/env python3
# Copyright (c) 2017-2018 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test logic for setting nMinimumChainWork on command line.

Nodes don't consider themselves out of "initial block download" until
their active chain has more work than nMinimumChainWork.

Nodes don't download blocks from a peer unless the peer's best known block
has more work than nMinimumChainWork.

While in initial block download, nodes won't relay blocks to their peers, so
test that this parameter functions as intended by verifying that block relay
only succeeds past a given node once its nMinimumChainWork has been exceeded.

Nodes answer a getheaders request once their active chain has more work than
nMinimumChainWork, also while they are still in initial block download. Before
that they only answer peers that have the download permission.
"""

import time

from test_framework.messages import msg_getheaders
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, p2p_port

# 2 hashes required per regtest block (with no difficulty adjustment)
REGTEST_WORK_PER_BLOCK = 2

class MinimumChainWorkTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3

        self.extra_args = [[], ["-minimumchainwork=0x65"], ["-minimumchainwork=0x65"]]
        self.node_min_work = [0, 101, 101]

    def setup_network(self):
        # This test relies on the chain setup being:
        # node0 <- node1 <- node2
        # Before leaving IBD, nodes prefer to download blocks from outbound
        # peers, so ensure that we're mining on an outbound peer and testing
        # block relay to inbound peers.
        #
        # node2 listens on a second port as well; a peer that connects to that
        # port is given the download permission.
        self.permitted_port = p2p_port(self.num_nodes)
        self.extra_args[2] = self.extra_args[2] + ["-whitebind=download@127.0.0.1:{}".format(self.permitted_port)]
        self.setup_nodes()
        for i in range(self.num_nodes-1):
            self.connect_nodes(i+1, i)

        # Set clock of node2 2 days ahead, to keep it in IBD during this test.
        self.nodes[2].setmocktime(int(time.time()) + 48*60*60)

    def run_test(self):
        # Start building a chain on node0.  node2 shouldn't be able to sync until node1's
        # minchainwork is exceeded
        starting_chain_work = REGTEST_WORK_PER_BLOCK # Genesis block's work
        self.log.info("Testing relay across node %d (minChainWork = %d)", 1, self.node_min_work[1])

        starting_blockcount = self.nodes[2].getblockcount()

        num_blocks_to_generate = int((self.node_min_work[1] - starting_chain_work) / REGTEST_WORK_PER_BLOCK)
        self.log.info("Generating %d blocks on node0", num_blocks_to_generate)
        hashes = self.nodes[0].generatetoaddress(num_blocks_to_generate,
                                                 self.nodes[0].get_deterministic_priv_key().address)

        self.log.info("Node0 current chain work: %s", self.nodes[0].getblockheader(hashes[-1])['chainwork'])

        # Sleep a few seconds and verify that node2 didn't get any new blocks
        # or headers.  We sleep, rather than sync_blocks(node0, node1) because
        # it's reasonable either way for node1 to get the blocks, or not get
        # them (since they're below node1's minchainwork).
        time.sleep(3)

        self.log.info("Verifying node 2 has no more blocks than before")
        self.log.info("Blockcounts: %s", [n.getblockcount() for n in self.nodes])
        # Node2 shouldn't have any new headers yet, because node1 should not
        # have relayed anything.
        assert_equal(len(self.nodes[2].getchaintips()), 1)
        assert_equal(self.nodes[2].getchaintips()[0]['height'], 0)

        assert self.nodes[1].getbestblockhash() != self.nodes[0].getbestblockhash()
        assert_equal(self.nodes[2].getblockcount(), starting_blockcount)

        self.log.info("Check that getheaders requests to node2 are ignored")
        peer = self.nodes[2].add_p2p_connection(P2PInterface())
        msg = msg_getheaders()
        msg.locator.vHave = [int(self.nodes[2].getbestblockhash(), 16)]
        msg.hashstop = 0
        # The node handles a peer's messages in order: a reply to the
        # getheaders would arrive before the reply to the ping.
        with self.nodes[2].assert_debug_log(["Ignoring getheaders from peer="]):
            peer.send_and_ping(msg)
        assert "headers" not in peer.last_message

        self.log.info("Check that node2 answers a peer that has the download permission")
        permitted_peer = self.nodes[2].add_p2p_connection(P2PInterface(), dstport=self.permitted_port)
        assert "download" in self.nodes[2].getpeerinfo()[-1]["permissions"]
        permitted_peer.send_and_ping(msg)
        # node2 has no block after the one the request names.
        assert_equal(permitted_peer.last_message["headers"].headers, [])

        self.log.info("Generating one more block")
        self.nodes[0].generatetoaddress(1, self.nodes[0].get_deterministic_priv_key().address)

        self.log.info("Verifying nodes are all synced")

        # Because nodes in regtest are all manual connections (eg using
        # addnode), node1 should not have disconnected node0. If not for that,
        # we'd expect node1 to have disconnected node0 for serving an
        # insufficient work chain, in which case we'd need to reconnect them to
        # continue the test.

        self.sync_all()
        self.log.info("Blockcounts: %s", [n.getblockcount() for n in self.nodes])

        self.log.info("Test that getheaders requests to node2 are not ignored")
        peer.send_and_ping(msg)
        assert "headers" in peer.last_message
        # The request named the genesis block, so the reply holds the rest of the chain.
        assert_equal(len(peer.last_message["headers"].headers), self.nodes[2].getblockcount())

        # Verify that node2 is in fact still in IBD (otherwise this test may
        # not be exercising the logic we want!)
        assert_equal(self.nodes[2].getblockchaininfo()['initialblockdownload'], True)

if __name__ == '__main__':
    MinimumChainWorkTest().main()
