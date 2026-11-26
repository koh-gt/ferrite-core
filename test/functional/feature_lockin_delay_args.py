#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a malformed number of locked-in windows in -vbparams stops the node from starting.

The sixth field of -vbparams gives the number of windows a deployment stays locked in before its
rules apply. It must be a whole number of at least 1. feature_lockin_delay.py tests the deployment
itself; this test only checks the start-up error, so that the other test can also run with programs
that report start-up errors in a window instead of exiting.
"""

from test_framework.test_framework import BitcoinTestFramework

START_HEIGHT = 144
TIMEOUT_HEIGHT = 99999999


class LockInDelayArgsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.common_args = ["-vbparams=mweb:-2:0", "-par=1"]

    def run_test(self):
        self.log.info("A malformed number of locked-in windows stops the node from starting")
        self.stop_node(0)
        for value in ["0", "-1", "x"]:
            self.nodes[0].assert_start_raises_init_error(
                extra_args=self.common_args + ["-vbparams=p2mr:0:0:{}:{}:{}".format(START_HEIGHT, TIMEOUT_HEIGHT, value)],
                expected_msg="Error: Invalid nLockedInPeriods ({}), expecting a whole number of at least 1".format(value))


if __name__ == '__main__':
    LockInDelayArgsTest().main()
