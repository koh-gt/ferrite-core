#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the signalled activation of the MWEB kernel feature rules (the mwebkernel deployment).

Node 0 mines with the default block version, so it signals every started deployment, like a
pool running v3.3.0. Node 1 mines with -blockversion=0x20000000, like a pool that does not
signal. Both nodes use -vbparams to turn the deployment from always active into height-based
signalling that starts at START_HEIGHT (regtest windows are 144 blocks, 108 of them lock in).

The test has no kernel signing key, so it cannot build a validly signed kernel that sets the
pegout or extra-data feature without the field. Instead it sets the feature on a template kernel
and commits the changed kernel through the kernel root and the HogEx. While the rules do not
apply, that block gets as far as the signature check and fails there ("bad-blk-mweb"). Once they
apply, it fails earlier with "bad-mweb-empty-pegout" or "bad-mweb-empty-extradata".
"""

import copy
import struct
from decimal import Decimal

from test_framework.blocktools import NORMAL_GBT_REQUEST_PARAMS, create_coinbase
from test_framework.fec_util import setup_mweb_chain
from test_framework.messages import (
    CBlock,
    CTransaction,
    FromHex,
    Hash,
    MWEBBlock,
    blake3,
    ser_compact_size,
)
from test_framework.script_util import hogaddr_script
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

START_HEIGHT = 4 * 144
TIMEOUT_HEIGHT = 99999999
WINDOW = 144
THRESHOLD = 108
DEPLOYMENT_BIT = 5
NOT_SIGNALLING_VERSION = 0x20000000
PEGOUT_FEATURE_BIT = 0x04
EXTRA_DATA_FEATURE_BIT = 0x20


def node_serialization(kernel):
    """Kernel bytes as the node serializes them (Kernel::Serialize): an empty pegout list or empty
    extra data is left out even when its feature bit is set."""
    written = copy.deepcopy(kernel)
    if not written.pegouts:
        written.features &= ~PEGOUT_FEATURE_BIT
    if not written.extradata:
        written.features &= ~EXTRA_DATA_FEATURE_BIT
    return bytes([kernel.features]) + written.serialize()[1:]


def mmr_root(leaves):
    """Root of an MWEB MMR holding the given leaf data (libmw MemMMR and IMMR::Root)."""
    def height(pos):
        h, peak = pos, (1 << pos.bit_length()) - 1
        while peak:
            if h >= peak:
                h -= peak
            peak >>= 1
        return h

    def parent(pos, left, right):
        return blake3(struct.pack("<Q", pos) + left + right).serialize()

    nodes = []
    for data in leaves:
        nodes.append(blake3(struct.pack("<Q", len(nodes)) + ser_compact_size(len(data)) + data).serialize())
        while height(len(nodes)) > 0:
            pos = len(nodes)
            nodes.append(parent(pos, nodes[pos - (1 << height(pos))], nodes[-1]))

    peaks, peak_size, start = [], (1 << len(nodes).bit_length()) - 1, 0
    while peak_size:
        if len(nodes) - start >= peak_size:
            peaks.append(start + peak_size - 1)
            start += peak_size
        peak_size >>= 1

    root = None
    for pos in reversed(peaks):
        root = nodes[pos] if root is None else parent(len(nodes), nodes[pos], root)
    return root


class MWEBKernelActivationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        vbparams = "-vbparams=mwebkernel:0:0:{}:{}".format(START_HEIGHT, TIMEOUT_HEIGHT)
        self.extra_args = [
            [vbparams],
            [vbparams, "-blockversion={}".format(NOT_SIGNALLING_VERSION)],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def deployment(self):
        return self.nodes[0].getblockchaininfo()['softforks']['mwebkernel']

    def template(self):
        # Segwit-only callers, as pools use (NORMAL_GBT_REQUEST_PARAMS lists only segwit)
        return self.nodes[0].getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)

    def run_test(self):
        node, other_pool = self.nodes

        self.log.info("Activate MWEB, then stop two blocks before signalling starts")
        setup_mweb_chain(node)
        node.generate(START_HEIGHT - 2 - node.getblockcount())
        self.sync_blocks()
        self.check_kernel_rules(active=False)
        assert_equal(self.deployment(), {
            'type': 'bip8',
            'bip8': {'status': 'defined', 'start_height': START_HEIGHT, 'timeout_height': TIMEOUT_HEIGHT, 'since': 0},
            'active': False,
        })
        tmpl = self.template()
        assert 'mwebkernel' not in tmpl['vbavailable']
        assert 'mwebkernel' not in tmpl['rules']
        assert_equal(tmpl['version'] & (1 << DEPLOYMENT_BIT), 0)

        self.log.info("Signalling starts at the start height, and templates for segwit-only callers set bit 5")
        node.generate(1)
        self.sync_blocks()
        info = self.deployment()['bip8']
        assert_equal((info['status'], info['bit'], info['since']), ('started', DEPLOYMENT_BIT, START_HEIGHT))
        tmpl = self.template()
        assert_equal(tmpl['vbavailable']['mwebkernel'], DEPLOYMENT_BIT)
        assert 'mwebkernel' not in tmpl['rules']
        assert tmpl['version'] & (1 << DEPLOYMENT_BIT)

        self.log.info("A window with 37 blocks from a pool that does not signal does not lock in")
        other_pool.generate(WINDOW - THRESHOLD + 1)
        self.sync_blocks()
        stats = self.deployment()['bip8']['statistics']
        assert_equal((stats['elapsed'], stats['count'], stats['possible']), (WINDOW - THRESHOLD + 1, 0, False))
        node.generate(THRESHOLD - 1)
        self.sync_blocks()
        assert_equal(node.getblockcount(), START_HEIGHT + WINDOW - 1)
        info = self.deployment()['bip8']
        assert_equal((info['status'], info['since']), ('started', START_HEIGHT))

        self.log.info("A window in which every block signals locks in; the rules do not apply yet")
        node.generate(WINDOW)
        self.sync_blocks()
        info = self.deployment()['bip8']
        assert_equal((info['status'], info['since']), ('locked_in', START_HEIGHT + 2 * WINDOW))
        assert self.template()['version'] & (1 << DEPLOYMENT_BIT)
        node.generate(WINDOW - 1)
        self.sync_blocks()
        # The next block is the last one of the locked-in window
        assert_equal(node.getblockcount(), START_HEIGHT + 3 * WINDOW - 2)
        self.check_kernel_rules(active=False)

        self.log.info("The rules apply from the first block after the locked-in window")
        node.generate(1)
        self.sync_blocks()
        activation_height = START_HEIGHT + 3 * WINDOW
        assert_equal(node.getblockcount(), activation_height - 1)
        self.check_kernel_rules(active=True)
        assert_equal(self.deployment(), {
            'type': 'bip8',
            'bip8': {'status': 'active', 'start_height': START_HEIGHT, 'timeout_height': TIMEOUT_HEIGHT, 'since': activation_height},
            'height': activation_height,
            'active': True,
        })
        tmpl = self.template()
        assert 'mwebkernel' in tmpl['rules']
        assert 'mwebkernel' not in tmpl['vbavailable']
        assert_equal(tmpl['version'] & (1 << DEPLOYMENT_BIT), 0)

        self.log.info("The pool that never signalled still mines valid blocks after activation")
        node.generate(1)
        self.sync_blocks()
        other_pool.generate(2)
        self.sync_blocks()
        assert_equal(node.getbestblockhash(), other_pool.getbestblockhash())
        assert_equal(node.getblockcount(), activation_height + 2)

    def check_kernel_rules(self, active):
        # getblocktemplate reuses its template for 5 seconds while the tip is unchanged, so this must
        # be the first template request at the current tip for the new MWEB transaction to show up.
        node = self.nodes[0]
        node.sendtoaddress(node.getnewaddress(address_type='mweb'), Decimal("0.1"))
        assert_equal(self.propose(feature=None), None)
        for feature, reason in ((PEGOUT_FEATURE_BIT, 'bad-mweb-empty-pegout'),
                                (EXTRA_DATA_FEATURE_BIT, 'bad-mweb-empty-extradata')):
            if active:
                assert_equal(self.propose(feature), reason)
            else:
                with node.assert_debug_log(["ContextualCheckBlock: bad-blk-mweb, BlockValidator::ValidateBlock failed"]):
                    assert_equal(self.propose(feature), 'bad-blk-mweb')

    def propose(self, feature):
        """Proposes the current template with the pegout or extra-data feature set but empty on one
        kernel, committed through the kernel root and the HogEx, and returns the node's verdict.
        With feature=None the template is proposed unchanged, through the same steps."""
        node = self.nodes[0]
        gbt = self.template()
        mweb_block = FromHex(MWEBBlock(), gbt['mweb'])
        kernels = mweb_block.body.kernels
        assert len(kernels) > 0
        assert_equal(mmr_root([node_serialization(k) for k in kernels]), mweb_block.header.kernel_root.serialize())

        if feature is not None:
            kernel = next(k for k in kernels if not k.features & (PEGOUT_FEATURE_BIT | EXTRA_DATA_FEATURE_BIT))
            kernel.features |= feature
            if feature == PEGOUT_FEATURE_BIT:
                kernel.pegouts = []
            else:
                kernel.extradata = b""
            kernel.rehash()
        mweb_block.header.kernel_root = Hash.from_byte_arr(mmr_root([node_serialization(k) for k in kernels]))
        mweb_block.rehash()

        txs = [FromHex(CTransaction(), tx['data']) for tx in gbt['transactions']]
        hogex = txs[-1]
        assert hogex.hogex
        hogex.vout[0].scriptPubKey = hogaddr_script(mweb_block.header.hash.to_byte_arr())
        hogex.rehash()

        block = CBlock()
        block.nVersion = gbt['version']
        block.hashPrevBlock = int(gbt['previousblockhash'], 16)
        block.nTime = gbt['curtime']
        block.nBits = int(gbt['bits'], 16)
        block.nNonce = 0
        block.vtx = [create_coinbase(height=int(gbt['height'])), *txs]
        block.mweb_block = mweb_block
        block.hashMerkleRoot = block.calc_merkle_root()
        return node.getblocktemplate(template_request={
            'data': block.serialize().hex(),
            'mode': 'proposal',
            'rules': ['segwit'],
        })


if __name__ == '__main__':
    MWEBKernelActivationTest().main()
