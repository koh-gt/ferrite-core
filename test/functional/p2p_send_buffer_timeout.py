#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the timeout for peers whose send buffer stays full.

Once the node's send buffer for a peer is full, the node stops processing the
peer's messages until the buffer drains. Check that the peer stays connected
while its send buffer has been full for the timeout interval (20 minutes),
and is disconnected once it has been full for longer.
"""

import time

from test_framework.messages import CInv, MSG_BLOCK, msg_getdata, ser_compact_size
from test_framework.p2p import NetworkThread, P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than

# From net.h and net_processing.cpp
TIMEOUT_INTERVAL = 20 * 60
DEFAULT_MAXSENDBUFFER = 1000 * 1000  # bytes
MAX_INV_SZ = 50000


class msg_getdata_quick(msg_getdata):
    def serialize(self):
        # Join the entries once; appending them one by one takes seconds here.
        return ser_compact_size(len(self.inv)) + b"".join(inv.serialize() for inv in self.inv)


class P2PStopReading(P2PInterface):
    def stop_reading(self):
        """Stop reading from the socket, so that the node's sends to us back up."""
        NetworkThread.network_event_loop.call_soon_threadsafe(self._transport.pause_reading)


class SendBufferTimeoutTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [['-peertimeout=3']]

    def mock_forward(self, delta):
        self.mock_time += delta
        self.nodes[0].setmocktime(self.mock_time)

    def queued_bytes(self, msg_type):
        """Bytes of messages of this type the node has queued for the peer so far."""
        return self.nodes[0].getpeerinfo()[0]['bytessent_per_msg'].get(msg_type, 0)

    def unsent_bytes(self):
        """Bytes the node has queued for the peer but not written to the socket."""
        info = self.nodes[0].getpeerinfo()[0]
        return sum(info['bytessent_per_msg'].values()) - info['bytessent']

    def queue_ping(self):
        """Have the node queue a ping for the peer, and wait until it has.

        The node queues the ping in a pass of its message handler that first
        serves the peer's next request, unless the send buffer is full.
        """
        pings = self.queued_bytes('ping')
        self.nodes[0].ping()
        self.wait_until(lambda: self.queued_bytes('ping') > pings)

    def run_test(self):
        node = self.nodes[0]
        self.mock_time = int(time.time())
        self.mock_forward(0)
        tip = int(node.generate(2)[-1], 16)

        peer = node.add_p2p_connection(P2PStopReading())

        self.log.info("Fill the node's send buffer for the peer")
        peer.stop_reading()
        # Ask for far more than the socket buffers and the send buffer hold.
        getdata = msg_getdata_quick([CInv(MSG_BLOCK, tip)] * MAX_INV_SZ)
        peer.send_message(getdata)
        peer.send_message(getdata)
        # Wait until the send buffer is full. After two pings, the message
        # handler has gone through the peer at least once since the block
        # bytes were read, and it serves a block each time unless the send
        # buffer is full.
        while True:
            blocks = self.queued_bytes('block')
            self.queue_ping()
            self.queue_ping()
            if self.queued_bytes('block') == blocks:
                break
        # The node stopped because its send buffer is full, not because it
        # served every request.
        assert_greater_than(self.unsent_bytes(), DEFAULT_MAXSENDBUFFER // 2)

        self.log.info("Check that the peer stays connected for the timeout interval")
        self.mock_forward(TIMEOUT_INTERVAL)
        # Restart the ping timer, so that the ping timeout does not apply
        # before the end of the test. Queuing more data must not restart the
        # timer for the full send buffer.
        self.queue_ping()
        time.sleep(4)  # longer than -peertimeout
        assert_equal(len(node.getpeerinfo()), 1)

        self.log.info("Check that the peer is disconnected once the timeout interval has passed")
        with node.assert_debug_log([f"send buffer full for {TIMEOUT_INTERVAL + 1}s, disconnecting peer=0"]):
            self.mock_forward(1)
            self.wait_until(lambda: len(node.getpeerinfo()) == 0)

        # The test connection does not notice the disconnection while it is
        # not reading; close it.
        peer.peer_disconnect()
        peer.wait_for_disconnect()


if __name__ == '__main__':
    SendBufferTimeoutTest().main()
