#!/usr/bin/env python3
# Copyright (c) 2021 The Bitcoin Core developers
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Test p2p addr-fetch connections

An addr-fetch connection is a short-lived outbound connection that the node
opens to ask a peer for addresses (for -seednode, and for a DNS seed that it
cannot look up directly). Check that the node keeps the connection until the
peer has sent an addr message with more than one address, and that it gives
up after five minutes. Then let the node fetch from a listening node: such a
node first announces its own address and answers getaddr some time later.
Check that the fetching node is still connected after the announcement, that
it receives the addresses it asked for, and that it disconnects then.
"""

import time

from test_framework.messages import msg_addr, CAddress, NODE_NETWORK, NODE_WITNESS
from test_framework.p2p import P2PInterface, p2p_lock
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, p2p_port

ADDR = CAddress()
ADDR.time = int(time.time())
ADDR.nServices = NODE_NETWORK | NODE_WITNESS
ADDR.ip = "192.0.0.8"
ADDR.port = 19444

# The address that the listening node announces as its own, and the
# addresses that it knows.
LISTENER_IP = "251.0.0.1"
KNOWN_IPS = ["250.{}.0.1".format(i) for i in range(1, 51)]
# A node answers getaddr with at most this share of the addresses it knows.
MAX_PCT_ADDR_TO_SEND = 23


class P2PAddrFetch(BitcoinTestFramework):

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        # Neither node tries to connect to the addresses it learns.
        self.extra_args = [
            ["-connect=0"],
            ["-connect=0", "-listen=1", "-externalip={}".format(LISTENER_IP)],
        ]

    def setup_network(self):
        self.setup_nodes()

    def known_addresses(self):
        return sorted(a['address'] for a in self.nodes[0].getnodeaddresses(0))

    def test_test_peer(self):
        node = self.nodes[0]
        self.log.info("Connect to an addr-fetch peer")
        peer = node.add_outbound_p2p_connection(P2PInterface(), p2p_idx=0, connection_type="addr-fetch")
        info = node.getpeerinfo()
        assert_equal(len(info), 1)
        assert_equal(info[0]['connection_type'], 'addr-fetch')

        self.log.info("Check that we send getaddr but don't try to sync headers with the addr-fetch peer")
        peer.sync_with_ping()
        peer.sync_with_ping()
        with p2p_lock:
            assert peer.message_count['getaddr'] == 1
            assert peer.message_count['getheaders'] == 0

        self.log.info("Check that answering the getaddr with a single address does not lead to disconnect")
        # This prevents disconnecting on self-announcements
        msg = msg_addr()
        msg.addrs = [ADDR]
        peer.send_and_ping(msg)
        assert_equal(len(node.getpeerinfo()), 1)

        self.log.info("Check that answering with larger addr messages leads to disconnect")
        msg.addrs = [ADDR] * 2
        with node.assert_debug_log(["addrfetch connection completed peer=0; disconnecting"]):
            peer.send_message(msg)
            peer.wait_for_disconnect(timeout=5)

        self.log.info("Check timeout for addr-fetch peer that does not send addrs")
        peer = node.add_outbound_p2p_connection(P2PInterface(), p2p_idx=1, connection_type="addr-fetch")
        node.setmocktime(int(time.time()) + 150)
        peer.sync_with_ping()
        peer.sync_with_ping()
        with node.assert_debug_log(["addrfetch connection timeout; disconnecting peer=1"]):
            node.setmocktime(int(time.time()) + 301)  # Timeout: 5 minutes
            peer.wait_for_disconnect(timeout=5)
        node.setmocktime(0)
        self.wait_until(lambda: len(node.getpeerinfo()) == 0)

    def test_listening_node(self):
        node, listener = self.nodes

        self.log.info("Let the listening node leave initial block download and learn some addresses")
        # A node announces its own address only after it has left initial
        # block download.
        listener.generatetoaddress(1, listener.get_deterministic_priv_key().address)
        assert not listener.getblockchaininfo()['initialblockdownload']
        known = [ip for ip in KNOWN_IPS if listener.addpeeraddress(ip, 19444)['success']]
        # Enough for an answer to getaddr with several addresses.
        assert len(known) * MAX_PCT_ADDR_TO_SEND // 100 > 2
        # With its clock stopped, the listening node does not send the answer
        # to getaddr yet.
        now = int(time.time())
        listener.setmocktime(now)

        self.log.info("Fetch addresses from the listening node")
        before = self.known_addresses()
        node.addconnection("127.0.0.1:{}".format(p2p_port(1)), "addr-fetch")

        self.log.info("Check that the node stays connected when the listening node announces its own address")
        # The listening node sends its own address as soon as the connection
        # is up, in an addr message of its own.
        self.wait_until(lambda: self.known_addresses() == sorted(before + [LISTENER_IP]))
        assert_equal([peer['connection_type'] for peer in node.getpeerinfo()], ['addr-fetch'])

        self.log.info("Check that the node receives the answer to getaddr and then disconnects")

        def learned():
            return set(self.known_addresses()) - set(before) - {LISTENER_IP}

        with node.assert_debug_log(["addrfetch connection completed peer=2; disconnecting"]):
            # Make the listening node's next addr message due.
            listener.setmocktime(now + 15 * 60)
            self.wait_until(lambda: len(learned()) > 1)
            self.wait_until(lambda: len(node.getpeerinfo()) == 0)
        self.wait_until(lambda: len(listener.getpeerinfo()) == 0)
        assert learned() <= set(known)
        listener.setmocktime(0)

    def run_test(self):
        self.test_test_peer()
        self.test_listening_node()


if __name__ == '__main__':
    P2PAddrFetch().main()
