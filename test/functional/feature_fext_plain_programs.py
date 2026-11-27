#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a node program without the OP_RETURN index knows none of its options.

Only ferrite-qt-fext keeps an OP_RETURN index; ferrited does not. The test
runs when the node program is not ferrite-qt-fext. It checks that the help
names none of the index's options; that the program refuses each of their
names on the command line, as it refuses every option it does not know; that
it starts with them in ferrite.conf and ignores each with a line in the log;
that it writes no file of the index; and that no RPC call names the index.
"""

import os
import subprocess

from test_framework.test_framework import BitcoinTestFramework, SkipTest

# The options of the index, under both of their names, with a value each may take.
OPTIONS = [
    ('opreturnindex', '1'),
    ('opreturnmaxsize', '2048'),
    ('opreturnreindex', '1'),
    ('fextindex', '1'),
    ('fextmaxsize', '2048'),
    ('fextreindex', '1'),
]


def is_fext_program(binary):
    """Whether the node program is ferrite-qt-fext, the one program that keeps an OP_RETURN index."""
    return os.path.basename(binary).startswith('ferrite-qt-fext')


class PlainProgramsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        if is_fext_program(self.options.bitcoind):
            raise SkipTest("the node program is ferrite-qt-fext, which keeps an OP_RETURN index")

    def run_test(self):
        node = self.nodes[0]

        self.log.info("The help names none of the index's options")
        help_text = subprocess.run([self.options.bitcoind, '-help'], stdout=subprocess.PIPE, timeout=120).stdout.decode()
        assert '\n  -datacarrier' in help_text
        for name, _ in OPTIONS:
            assert '-' + name not in help_text, name

        self.log.info("Each name is refused on the command line")
        self.stop_node(0)
        for name, value in OPTIONS:
            argument = '-{}={}'.format(name, value)
            node.assert_start_raises_init_error(
                [argument], 'Error: Error parsing command line arguments: Invalid parameter {}'.format(argument))

        self.log.info("In ferrite.conf each name is ignored, with a line in the log")
        with open(os.path.join(node.datadir, 'ferrite.conf'), 'a', encoding='utf-8') as f:
            for name, value in OPTIONS:
                f.write('{}={}\n'.format(name, value))
        expected = ['Ignoring unknown configuration value {}.{}'.format(self.chain, name) for name, _ in OPTIONS]
        with node.assert_debug_log(expected_msgs=expected, unexpected_msgs=['OP_RETURN index']):
            self.start_node(0)
            node.generate(2)

        self.log.info("The program writes no file of the index")
        datadir = os.path.join(node.datadir, self.chain)
        for path in [os.path.join('indexes', 'opreturn'), os.path.join('indexes', 'fext'), 'fext']:
            assert not os.path.exists(os.path.join(datadir, path)), path

        self.log.info("No RPC call names the index")
        assert 'opreturn' not in node.help().lower()


if __name__ == '__main__':
    PlainProgramsTest().main()
