#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that peers.dat is written with a matching checksum while addresses are added.

The node writes its address manager to peers.dat from the scheduler thread
while other threads keep adding addresses. The file ends with a checksum of
its content; a node that finds the two not to match at startup discards the
file and starts without any known peers.

Add addresses from several RPC clients, have the node write peers.dat again
and again meanwhile, and check that every file's checksum matches its content.
Then start the node on the last file written that way and check that it loads
the addresses.
"""

import os
import threading

from test_framework.messages import hash256
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, get_rpc_proxy

DUMP_PEERS_INTERVAL = 15 * 60  # seconds, from net.cpp
CLIENTS = 3
ADDRESSES_BEFORE_FIRST_WRITE = 1000  # per client
WRITES = 20


class PeersDatChecksumTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # The node must not try to connect to the addresses it is given.
        self.extra_args = [["-connect=0"]]

    def setup_nodes(self):
        # Where the platform allows it, let the node run on one processor
        # only. A thread that waits for the address manager then runs as soon
        # as the thread that writes peers.dat lets go of it, also in the
        # middle of a write. With more processors that is rare.
        if hasattr(os, "sched_setaffinity"):
            processors = os.sched_getaffinity(0)
            os.sched_setaffinity(0, {min(processors)})
            try:
                super().setup_nodes()
            finally:
                os.sched_setaffinity(0, processors)
        else:
            super().setup_nodes()

    def write_peers_dat(self):
        """Have the node write peers.dat now and return the content of the file."""
        node = self.nodes[0]
        with node.assert_debug_log(["addresses to peers.dat"]):
            # The scheduler thread writes the file every DUMP_PEERS_INTERVAL.
            # Make the next write due, then wait until that thread has done
            # all that is due.
            node.mockscheduler(DUMP_PEERS_INTERVAL)
            node.syncwithvalidationinterfacequeue()
        with open(os.path.join(node.datadir, self.chain, "peers.dat"), "rb") as f:
            return f.read()

    def write_peers_dat_under_load(self):
        """Write peers.dat WRITES times while addresses are added; return the last file."""
        node = self.nodes[0]
        stop = threading.Event()
        added = [0] * CLIENTS
        errors = []

        def add_addresses(client):
            # A connection serves one call at a time, so every client has its
            # own. It is closed when the thread ends; a connection left open
            # would hold up the node's shutdown.
            rpc = get_rpc_proxy(node.url, node.index, timeout=node.rpc_timeout)
            try:
                n = 0
                while not stop.is_set():
                    # Addresses in many different /16 networks: the address
                    # manager spreads them over its buckets and keeps most.
                    rpc.addpeeraddress("{}.{}.{}.1".format(11 + 30 * client + (n >> 8) % 30, n & 255, (n >> 13) & 255), 8333)
                    n += 1
                    added[client] = n
            except Exception as e:
                errors.append(e)

        threads = [threading.Thread(target=add_addresses, args=(c,)) for c in range(CLIENTS)]
        for t in threads:
            t.start()
        try:
            self.wait_until(lambda: errors or min(added) >= ADDRESSES_BEFORE_FIRST_WRITE)
            for i in range(WRITES):
                peers_dat = self.write_peers_dat()
                if hash256(peers_dat[:-32]) != peers_dat[-32:]:
                    raise AssertionError("The checksum of peers.dat does not match its content (write {} of {}, {} bytes)".format(i + 1, WRITES, len(peers_dat)))
        finally:
            stop.set()
            for t in threads:
                t.join()
        assert_equal(errors, [])
        return peers_dat

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Write peers.dat %d times while %d clients add addresses", WRITES, CLIENTS)
        peers_dat = self.write_peers_dat_under_load()

        self.log.info("Start the node on the last of these files")
        self.stop_node(0)
        with open(os.path.join(node.datadir, self.chain, "peers.dat"), "wb") as f:
            f.write(peers_dat)
        with node.assert_debug_log(["addresses from peers.dat"], unexpected_msgs=["Invalid or missing peers.dat"]):
            self.start_node(0)
        assert node.getnodeaddresses(count=1)


if __name__ == '__main__':
    PeersDatChecksumTest().main()
