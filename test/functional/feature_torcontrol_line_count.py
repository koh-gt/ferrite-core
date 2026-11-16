#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the limit on the number of lines in a Tor control port reply.

The node talks to a mock Tor control port that records the commands it
receives and sends only the replies the test gives it. Check that a reply of
MAX_LINE_COUNT lines is processed, and that the node closes the control
connection, without processing the reply, when a reply has more lines.
"""

import socket
import threading

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, p2p_port

# From torcontrol.cpp
MAX_LINE_COUNT = 1000


class MockTorControlServer:
    """Accept one control connection, record its commands, send given replies."""

    def __init__(self, port):
        self.port = port
        self.commands = []
        self.conn = None
        self.closed = threading.Event()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", port))
        self.sock.listen(1)
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        buf = b""
        try:
            self.conn, _ = self.sock.accept()
            while True:
                data = self.conn.recv(4096)
                if not data:
                    break
                buf += data
                while b"\r\n" in buf:
                    line, buf = buf.split(b"\r\n", 1)
                    self.commands.append(line.decode())
        except OSError:
            pass
        self.closed.set()

    def send_reply(self, lines):
        """Send a 250 reply: '250-' before each line, '250 ' before the last one."""
        data = "".join("250{}{}\r\n".format(" " if i == len(lines) - 1 else "-", line) for i, line in enumerate(lines))
        self.conn.sendall(data.encode())

    def stop(self):
        if self.conn:
            self.conn.close()
        self.sock.close()
        self.thread.join(timeout=10)


class TorControlLineCountTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def run_test(self):
        node = self.nodes[0]
        tor = MockTorControlServer(p2p_port(self.num_nodes))
        self.restart_node(0, extra_args=["-torcontrol=127.0.0.1:{}".format(tor.port), "-listenonion=1", "-debug=tor"])
        self.wait_until(lambda: tor.commands == ["PROTOCOLINFO 1"])

        self.log.info("Check that a reply of MAX_LINE_COUNT lines is processed")
        reply = ["PROTOCOLINFO 1", "AUTH METHODS=NULL"] + ["Continuing"] * (MAX_LINE_COUNT - 3) + ["OK"]
        assert_equal(len(reply), MAX_LINE_COUNT)
        with node.assert_debug_log(expected_msgs=["tor: Using NULL authentication"], unexpected_msgs=["MAX_LINE_COUNT exceeded"]):
            tor.send_reply(reply)
            self.wait_until(lambda: len(tor.commands) == 2)
        assert_equal(tor.commands[1], "AUTHENTICATE")
        assert not tor.closed.is_set()

        self.log.info("Check that a reply of MAX_LINE_COUNT + 1 lines closes the connection")
        reply = ["Continuing"] * MAX_LINE_COUNT + ["OK"]
        with node.assert_debug_log(expected_msgs=["tor: Disconnecting because MAX_LINE_COUNT exceeded"]):
            tor.send_reply(reply)
            self.wait_until(lambda: tor.closed.is_set())
        # The reply was not passed on: a successful AUTHENTICATE reply would
        # have been followed by ADD_ONION.
        assert_equal(tor.commands, ["PROTOCOLINFO 1", "AUTHENTICATE"])

        self.log.info("Check that the node keeps running")
        node.getnetworkinfo()
        tor.stop()


if __name__ == '__main__':
    TorControlLineCountTest().main()
