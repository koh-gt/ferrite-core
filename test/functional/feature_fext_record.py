#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the OP_RETURN index of the node program that keeps one.

The test runs when the node program is ferrite-qt-fext, which keeps the index
by default while it has a wallet, and requires its help to name
-opreturnindex. It checks the index through the lines it writes to the debug
log, one for each state it enters and, with -debug=bench, one for each block
it writes, with the block's height and its number of OP_RETURN outputs.

It mines blocks with OP_RETURN outputs, replaces one of them in a
reorganisation, kills the node and starts it again, deletes the index at start
and builds it again, and checks the index's options in ferrite.conf and on the
command line, under both of their names, and without a wallet. The index never
fails, and no RPC call names it.
"""

import os
import re
import subprocess
import time

from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal

BLOCK_LINE = re.compile(r'OP_RETURN index: block (\d+), (\d+) rows, \d+ us')
RUNNING_LINE = re.compile(r'OP_RETURN index: state (RUNNING|BACKFILLING)$', re.MULTILINE)
FAILED_LINE = 'OP_RETURN index: state FAILED'
# The node's line when it deletes the database in the index's directory.
WIPE_LINE = re.compile(r'Wiping LevelDB in .*[/\\]indexes[/\\]opreturn$', re.MULTILINE)


def is_fext_program(binary):
    """Whether the node program is ferrite-qt-fext, the one program that keeps an OP_RETURN index."""
    return os.path.basename(binary).startswith('ferrite-qt-fext')


def names_index_option(binary):
    """Whether a node program's help names the OP_RETURN index's option."""
    result = subprocess.run([binary, '-help'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
    if result.returncode != 0:
        raise AssertionError('{} -help failed: {}'.format(binary, result.stderr.decode(errors='replace')))
    return b'\n  -opreturnindex\n' in result.stdout


class OpReturnIndexTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        if not is_fext_program(self.options.bitcoind):
            raise SkipTest("the node program is not ferrite-qt-fext, so it keeps no OP_RETURN index")

    def debug_log(self):
        with open(os.path.join(self.nodes[0].datadir, self.chain, 'debug.log'), encoding='utf-8') as f:
            return f.read()

    def log_since(self, position):
        return self.debug_log()[position:]

    def blocks_written(self, position):
        """The blocks the index wrote since a place in the log, as (height, rows) in the order written."""
        return [(int(h), int(n)) for h, n in BLOCK_LINE.findall(self.log_since(position))]

    def wait_for_block(self, position, height, rows):
        self.wait_until(lambda: (height, rows) in self.blocks_written(position))

    def send_message(self, text):
        """A transaction with one OP_RETURN output, whose data is the text, sent to the mempool."""
        node = self.nodes[0]
        raw = node.createrawtransaction([], [{'data': text.encode().hex()}])
        funded = node.fundrawtransaction(raw)
        signed = node.signrawtransactionwithwallet(funded['hex'])
        return node.sendrawtransaction(signed['hex'])

    def restart(self, extra_args=None):
        """Restarts the node; the place in the log where its new run begins."""
        self.stop_node(0)
        position = len(self.debug_log())
        self.start_node(0, extra_args=extra_args)
        return position

    def set_conf(self, lines):
        """Replaces the lines this test added to the node's ferrite.conf."""
        path = os.path.join(self.nodes[0].datadir, 'ferrite.conf')
        with open(path, encoding='utf-8') as f:
            kept = [line for line in f.read().splitlines() if not line.startswith(('opreturn', 'fext'))]
        with open(path, 'w', encoding='utf-8') as f:
            f.write('\n'.join(kept + lines) + '\n')

    def check_off(self, position):
        """The index is off in this run: it enters no running state and writes no block. An index that is on writes
        a block within milliseconds; the test gives it a second and a half."""
        self.nodes[0].generate(1)
        time.sleep(1.5)
        assert RUNNING_LINE.search(self.log_since(position)) is None
        assert_equal(self.blocks_written(position), [])

    def check_on(self, position, message=True):
        """The index runs in this run: it enters a running state and writes the next block, with a message in it
        when the node has a wallet to pay for one. (The wallet may send its unconfirmed transactions again at start,
        so the block may hold more.)"""
        node = self.nodes[0]
        self.wait_until(lambda: RUNNING_LINE.search(self.log_since(position)) is not None)
        height = node.getblockcount() + 1
        if message:
            self.send_message('on at {}'.format(height))
        node.generate(1)
        self.wait_until(lambda: any(h == height for h, _ in self.blocks_written(position)))
        rows = [n for h, n in self.blocks_written(position) if h == height]
        assert_equal(len(rows), 1)
        assert rows[0] >= 1 if message else rows[0] == 0, rows

    def run_test(self):
        node = self.nodes[0]
        assert names_index_option(self.options.bitcoind)

        self.log.info("The index is on by default, and writes every block it is told of")
        self.wait_until(lambda: RUNNING_LINE.search(self.debug_log()) is not None)
        position = len(self.debug_log())
        height = node.getblockcount()
        for text in ['first message', 'second message', 'third message']:
            self.send_message(text)
        node.generate(1)
        self.wait_for_block(position, height + 1, 3)
        node.generate(1)
        self.wait_for_block(position, height + 2, 0)

        # The log shows the blocks the index writes, not the rows it erases: that the rows of a disconnected block are
        # gone before those of the new branch are written is checked by the unit test
        # opreturn_record_tests/reorg_disconnect_connect.
        self.log.info("A block replaced in a reorganisation is written again with the new block's outputs")
        node.invalidateblock(node.getbestblockhash())
        node.invalidateblock(node.getbestblockhash())
        assert_equal(node.getblockcount(), height)
        self.send_message('fourth message')
        node.generate(1)
        self.wait_for_block(position, height + 1, 4)
        written = [b for b in self.blocks_written(position) if b[0] == height + 1]
        assert_equal(written, [(height + 1, 3), (height + 1, 4)])

        self.log.info("The index runs on after the node was killed, also where the node lost blocks it had written")
        node.process.kill()
        node.process.wait(timeout=60)
        node.running = False
        node.process = None
        node.rpc_connected = False
        node.rpc = None
        position = len(self.debug_log())
        self.start_node(0)
        self.check_on(position)

        self.log.info("-opreturnreindex deletes the index at start and builds it again from the blocks that connect")
        index_dir = os.path.join(node.datadir, self.chain, 'indexes', 'opreturn')
        # When LevelDB opens a database it keeps a file named LOG in the database's directory, and it deletes that file
        # with the database: one that the test leaves there stays after a restart and is gone once the index is deleted.
        marker = os.path.join(index_dir, 'LOG')
        self.stop_node(0)
        with open(marker, 'w', encoding='utf-8') as f:
            f.write('left by the test\n')
        position = len(self.debug_log())
        self.start_node(0)
        self.check_on(position)
        assert WIPE_LINE.search(self.log_since(position)) is None
        assert os.path.exists(marker)
        position = self.restart(['-opreturnreindex'])
        self.check_on(position)
        assert WIPE_LINE.search(self.log_since(position)) is not None
        assert not os.path.exists(marker)
        assert os.path.isdir(index_dir)
        assert not os.path.exists(os.path.join(node.datadir, self.chain, 'indexes', 'fext'))

        self.log.info("ferrite.conf can switch the index off, under either name of its option")
        for line in ['opreturnindex=0', 'fextindex=0']:
            self.set_conf([line])
            self.check_off(self.restart())
        self.log.info("... and an option on the command line wins over ferrite.conf")
        self.set_conf(['opreturnindex=0'])
        self.check_on(self.restart(['-opreturnindex=1']))
        self.set_conf([])

        self.log.info("Without a wallet the index is off, unless its option is set")
        self.check_off(self.restart(['-disablewallet']))
        self.check_on(self.restart(['-disablewallet', '-opreturnindex=1']), message=False)
        self.restart()

        self.log.info("No RPC call names the index, it never failed, and only -opreturnreindex deleted it")
        assert 'opreturn' not in node.help().lower()
        assert FAILED_LINE not in self.debug_log()
        assert_equal(len(WIPE_LINE.findall(self.debug_log())), 1)


if __name__ == '__main__':
    OpReturnIndexTest().main()
