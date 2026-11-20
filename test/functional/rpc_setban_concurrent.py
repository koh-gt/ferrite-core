#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that bans added by concurrent setban calls all reach banlist.dat.

A setban call writes the ban list to disk before it returns. Several clients
add one ban each at the same moment, again and again. Whenever all calls of
such a batch have returned, every ban added so far must be in banlist.dat, and
after a restart the node must still list all of them.
"""

import os
import socket
import threading

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, get_rpc_proxy

CLIENTS = 4  # the default number of RPC worker threads
BATCHES = 40


class SetBanConcurrentTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def bans_missing_on_disk(self, addresses):
        """Return those of the IPv4 addresses that banlist.dat does not hold."""
        with open(os.path.join(self.nodes[0].datadir, self.chain, "banlist.dat"), "rb") as f:
            data = f.read()
        # An IPv4 address is stored as an IPv4-mapped IPv6 address.
        return [a for a in addresses if b"\x00" * 10 + b"\xff\xff" + socket.inet_aton(a) not in data]

    def add_bans(self):
        """Add bans from all clients at once, BATCHES times; return the banned addresses."""
        node = self.nodes[0]
        # A connection serves one call at a time, so every client has its own.
        # They are closed when this function returns; a connection left open
        # would hold up the node's shutdown.
        clients = [get_rpc_proxy(node.url, node.index, timeout=node.rpc_timeout) for _ in range(CLIENTS)]
        errors = []

        def add_ban(rpc, address, barrier):
            try:
                barrier.wait()
                rpc.setban(address, "add")
            except Exception as e:
                errors.append(e)
                barrier.abort()

        banned = []
        for i in range(BATCHES):
            addresses = ["10.0.{}.{}".format(i, c + 1) for c in range(CLIENTS)]
            barrier = threading.Barrier(CLIENTS)
            threads = [threading.Thread(target=add_ban, args=(clients[c], addresses[c], barrier)) for c in range(CLIENTS)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            assert_equal(errors, [])
            banned += addresses
            assert_equal(self.bans_missing_on_disk(banned), [])
        return banned

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Add bans from %d clients at once, %d times", CLIENTS, BATCHES)
        expected = sorted(a + "/32" for a in self.add_bans())
        assert_equal(sorted(b["address"] for b in node.listbanned()), expected)

        self.log.info("Restart the node and check that every ban is still there")
        self.restart_node(0)
        assert_equal(sorted(b["address"] for b in node.listbanned()), expected)


if __name__ == '__main__':
    SetBanConcurrentTest().main()
