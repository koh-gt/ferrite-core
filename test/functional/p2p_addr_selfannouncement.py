#!/usr/bin/env python3
# Copyright (c) 2025-present The Bitcoin Core developers
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Test that a node sends a self-announcement with its external IP to
in- and outbound peers after connection open and again sometime later.

The node keeps a record, per peer, of the addresses that the peer knows
already and does not send those again. Its own address is in that record
after the first announcement, and must be sent again all the same when the
next announcement is due, about once a day.
"""

import time

from test_framework.messages import (
    CBlockHeader,
    FromHex,
    msg_headers,
)
from test_framework.p2p import P2PInterface, p2p_lock
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, p2p_port

IP_TO_ANNOUNCE = "251.0.0.1"
ONE_DAY = 60 * 60 * 24


class SelfAnnouncementReceiver(P2PInterface):
    def __init__(self, *, addrv2):
        super().__init__(support_addrv2=addrv2)
        self.addrv2_test = addrv2
        # One entry per addr or addrv2 message received: its addresses.
        self.addr_messages = []

    def handle_addr_message(self, message):
        self.addr_messages.append([(addr.ip, addr.port, addr.nServices, addr.time) for addr in message.addrs])

    def on_addrv2(self, message):
        assert self.addrv2_test
        self.handle_addr_message(message)

    def on_addr(self, message):
        assert not self.addrv2_test
        self.handle_addr_message(message)

    def received(self):
        with p2p_lock:
            return list(self.addr_messages)


class AddrSelfAnnouncementTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        # The node listens, but makes no connections of its own.
        self.extra_args = [["-connect=0", "-listen=1", "-externalip={}".format(IP_TO_ANNOUNCE)]]

    def run_test(self):
        node = self.nodes[0]
        # We only self-announce after initial block download is done
        node.generatetoaddress(1, node.get_deterministic_priv_key().address)
        assert not node.getblockchaininfo()["initialblockdownload"]

        netinfo = node.getnetworkinfo()
        assert_equal([(a["address"], a["port"]) for a in netinfo["localaddresses"]], [(IP_TO_ANNOUNCE, p2p_port(0))])
        self.services = int(netinfo["localservices"], 16)
        self.mocktime = int(time.time())
        node.setmocktime(self.mocktime)

        self.self_announcement_test(outbound=False, addrv2=False)
        self.self_announcement_test(outbound=False, addrv2=True)
        self.self_announcement_test(outbound=True, addrv2=False)
        self.self_announcement_test(outbound=True, addrv2=True)

    def self_announcement(self):
        """The node's own address as it announces it now."""
        return (IP_TO_ANNOUNCE, p2p_port(0), self.services, self.mocktime)

    def self_announcement_test(self, *, outbound, addrv2):
        node = self.nodes[0]
        connection_type = "outbound" if outbound else "inbound"
        addr_version = "addrv2" if addrv2 else "addrv1"
        advertising = "AdvertiseLocal: advertising address {}:{}".format(IP_TO_ANNOUNCE, p2p_port(0))

        self.log.info("Check that we get an initial self-announcement from the node ({}, {})".format(connection_type, addr_version))
        with node.assert_debug_log([advertising]):
            if outbound:
                addr_receiver = node.add_outbound_p2p_connection(SelfAnnouncementReceiver(addrv2=addrv2), p2p_idx=int(addrv2), connection_type="outbound-full-relay")
            else:
                addr_receiver = node.add_p2p_connection(SelfAnnouncementReceiver(addrv2=addrv2))
            addr_receiver.sync_with_ping()
        # We expect only the self-announcement.
        assert_equal(addr_receiver.received(), [[self.self_announcement()]])

        if outbound:
            # to avoid the node evicting the outbound peer, protect it by announcing the most recent header to it
            tip_header = FromHex(CBlockHeader(), node.getblockheader(node.getbestblockhash(), False))
            addr_receiver.send_and_ping(msg_headers([tip_header]))

        self.log.info("Check that we get more self-announcements sometime later ({}, {})".format(connection_type, addr_version))
        for _ in range(5):
            expected = addr_receiver.received()
            with p2p_lock:
                pings = addr_receiver.message_count["ping"]
            with node.assert_debug_log([advertising]):
                # m_next_local_addr_send and AVG_LOCAL_ADDRESS_BROADCAST_INTERVAL:
                # self-announcements are sent on an exponential distribution with mean interval of 24h.
                # Setting the mocktime 20d forward gives a probability of (1 - e^-(480/24)) that
                # the event will occur (i.e. this fails once in ~500 million repeats).
                self.mocktime += 20 * ONE_DAY
                node.setmocktime(self.mocktime)
                # The node sends a ping, which is due as well, before the
                # announcement. Wait for it and for the node to have seen
                # the answer: a ping that is still open when the clock is
                # moved again would count as timed out.
                addr_receiver.wait_until(lambda: addr_receiver.message_count["ping"] > pings)
                addr_receiver.sync_with_ping()
            # One more message, with the self-announcement only.
            expected.append([self.self_announcement()])
            assert_equal(addr_receiver.received(), expected)

        node.disconnect_p2ps()


if __name__ == '__main__':
    AddrSelfAnnouncementTest().main()
