#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that the node takes its time offset only from outbound peers.

The node takes one time sample per outbound peer IP address, from the time
in the peer's version message. Once it has four samples, and again at six,
eight and so on, it sets its time offset (getnetworkinfo "timeoffset") to
the median of the samples and its own zero offset, as long as that median
is within -maxtimeadjustment. Check that:

- inbound peers whose clocks are ahead do not change the offset, and
  getpeerinfo still shows each inbound peer's own offset;
- outbound peers whose clocks are behind do change it, one each of the
  full-relay, block-relay-only, feeler and addr-fetch connection types,
  and three of them are not enough.

Every peer needs its own IP address, so the test peers use the loopback
addresses 127.0.0.2 to 127.0.0.9.
"""

import socket
import time

from test_framework.messages import NODE_MWEB, NODE_NETWORK, NODE_WITNESS
from test_framework.p2p import NetworkThread, P2PInterface
from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal

INBOUND_OFFSET = 30 * 60
OUTBOUND_OFFSET = -20 * 60
INBOUND_IPS = ['127.0.0.2', '127.0.0.3', '127.0.0.4', '127.0.0.5']
OUTBOUND_PEERS = [
    ('127.0.0.6', 'outbound-full-relay'),
    ('127.0.0.7', 'block-relay-only'),
    ('127.0.0.8', 'feeler'),
    ('127.0.0.9', 'addr-fetch'),
]
SERVICES = NODE_NETWORK | NODE_WITNESS | NODE_MWEB


def can_bind(ip):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        try:
            s.bind((ip, 0))
        except OSError:
            return False
    return True


class TimeDataPeer(P2PInterface):
    """A test peer on its own IP address that sends a given time in its version message."""

    def __init__(self, ip, version_time):
        super().__init__()
        self.ip = ip
        self.version_time = version_time

    def peer_connect_send_version(self, services):
        super().peer_connect_send_version(services)
        self.on_connection_send_msg.nTime = self.version_time

    def peer_connect(self, dstaddr, dstport, *, net, timeout_factor, services=SERVICES):
        """Connect to the node from self.ip."""
        self.peer_connect_helper(dstaddr, dstport, net, timeout_factor)
        self.peer_connect_send_version(services)
        loop = NetworkThread.network_event_loop
        coroutine = loop.create_connection(lambda: self, host=dstaddr, port=dstport, local_addr=(self.ip, 0))
        return lambda: loop.call_soon_threadsafe(loop.create_task, coroutine)

    def peer_accept_connection(self, connect_id, connect_cb=lambda: None, *, net, timeout_factor, services=SERVICES):
        """Listen on self.ip for the node's outbound connection."""
        self.peer_connect_helper('0', 0, net, timeout_factor)
        self.peer_connect_send_version(services)
        return lambda: NetworkThread.listen(self, connect_cb, addr=self.ip, idx=connect_id)


class TimeDataTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        if not all(can_bind(ip) for ip in INBOUND_IPS + [ip for ip, _ in OUTBOUND_PEERS]):
            raise SkipTest("This test needs the loopback addresses 127.0.0.2 to 127.0.0.9.")

    def timeoffset(self):
        return self.nodes[0].getnetworkinfo()['timeoffset']

    def run_test(self):
        node = self.nodes[0]
        mock_time = int(time.time())
        node.setmocktime(mock_time)
        assert_equal(self.timeoffset(), 0)

        self.log.info("Inbound peers do not change the time offset")
        for ip in INBOUND_IPS:
            node.add_p2p_connection(TimeDataPeer(ip, mock_time + INBOUND_OFFSET))
        assert_equal(self.timeoffset(), 0)
        inbound = [p for p in node.getpeerinfo() if p['inbound']]
        assert_equal(sorted(p['addr'].split(':')[0] for p in inbound), INBOUND_IPS)
        assert_equal([p['timeoffset'] for p in inbound], [INBOUND_OFFSET] * len(INBOUND_IPS))

        self.log.info("Outbound peers change the time offset once there are four of them")
        for i, (ip, connection_type) in enumerate(OUTBOUND_PEERS):
            assert_equal(self.timeoffset(), 0)
            peer = TimeDataPeer(ip, mock_time + OUTBOUND_OFFSET)
            node.add_outbound_p2p_connection(peer, p2p_idx=i, connection_type=connection_type)
        assert_equal(self.timeoffset(), OUTBOUND_OFFSET)
        outbound = [p for p in node.getpeerinfo() if not p['inbound']]
        # The feeler connection has closed; the other three stay connected.
        assert_equal(sorted(p['connection_type'] for p in outbound), ['addr-fetch', 'block-relay-only', 'outbound-full-relay'])
        assert_equal([p['timeoffset'] for p in outbound], [OUTBOUND_OFFSET] * len(outbound))


if __name__ == '__main__':
    TimeDataTest().main()
