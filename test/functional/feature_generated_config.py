#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the ferrite.conf that a node writes into a new data directory.

The node first starts on a data directory that has no ferrite.conf, with the
test framework's settings given on the command line. The test checks that the
file the node writes holds only comments, blank lines, the [main] and [test]
section headers and addnode lines, with no addnode line outside those sections,
and that the addnode lines name only the project's own nodes;
that the node starts again on that file, without warnings and without taking
its addnode lines on regtest; and that the node leaves an existing ferrite.conf
as it is. It also checks that a section name the node does not recognize gives
a warning that names the file and the line.
"""

import os

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class GeneratedConfigTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.wallet_names = []

    def setup_network(self):
        # Do not start the node, so that its data directory stays new.
        self.add_nodes(self.num_nodes)

    def read_conf(self):
        with open(self.conf_path, encoding='utf8') as f:
            return f.read()

    def run_test(self):
        node = self.nodes[0]
        self.conf_path = os.path.join(node.datadir, 'ferrite.conf')

        self.log.info("Start a node on a data directory without ferrite.conf")
        framework_conf = self.read_conf()
        os.remove(self.conf_path)
        # The framework's settings, as command-line options.
        args = ['-' + line for line in framework_conf.splitlines() if '=' in line]
        self.start_node(0, extra_args=args)
        self.stop_node(0)
        assert os.path.isfile(self.conf_path)

        self.log.info("Check that the written file holds only comments, sections and addnode lines")
        generated = self.read_conf()
        section = None
        addnodes = {}
        comments = 0
        other_lines = []
        for line in generated.splitlines():
            line = line.strip()
            if not line:
                continue
            if line.startswith('#'):
                comments += 1
            elif line.startswith('[') and line.endswith(']'):
                section = line[1:-1]
                assert section in ('main', 'test'), line
            elif line.startswith('addnode=') and section is not None:
                addnodes.setdefault(section, []).append(line)
            else:
                other_lines.append(line)
        assert_equal(other_lines, [])
        assert_equal(sorted(addnodes), ['main', 'test'])
        # Only the project's own nodes are named.
        assert_equal(addnodes['main'], [
            'addnode=node1.ferritecoin.org',
            'addnode=node2.ferritecoin.org',
            'addnode=node3.ferritecoin.org',
            'addnode=118.189.201.104:9574',
            'addnode=118.189.201.104:9588',
            'addnode=118.189.201.104:9598',
        ])
        assert_equal(addnodes['test'], [
            'addnode=118.189.201.104:19574',
            'addnode=118.189.201.104:19588',
            'addnode=118.189.201.104:19598',
        ])
        assert comments > 0

        self.log.info("Start again on the written file")
        self.start_node(0, extra_args=args)
        # The [main] and [test] addnode lines do not apply to regtest.
        assert_equal(node.getaddednodeinfo(), [])
        self.stop_node(0)
        assert_equal(self.read_conf(), generated)

        self.log.info("Check that an existing ferrite.conf is left as it is")
        with open(self.conf_path, 'w', encoding='utf8') as f:
            f.write(framework_conf)
        self.start_node(0)
        self.stop_node(0)
        assert_equal(self.read_conf(), framework_conf)

        self.log.info("Check that an unrecognized section gives a warning")
        with open(self.conf_path, 'a', encoding='utf8') as f:
            f.write('[testnet]\n')
        self.start_node(0)
        # The warning gives the file name as -conf has it (the default, ferrite.conf).
        line = len(framework_conf.splitlines()) + 1
        self.stop_node(0, expected_stderr='Warning: ferrite.conf:{} Section [testnet] is not recognized.'.format(line))


if __name__ == '__main__':
    GeneratedConfigTest().main()
