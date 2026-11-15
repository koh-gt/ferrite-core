#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that blocks holding an MWEB kernel that sets a feature bit with an empty field read back.

The nodes run with the mwebkernel deployment not active. Node 0 accepts one block whose kernel sets
the pegout feature with no pegouts, and one whose kernel sets the extra-data feature with no extra
data. The test checks that node 0 returns each block from disk exactly as it was submitted,
that node 1 downloads both blocks from node 0, and that node 0 restarts with a full check of its
latest blocks and with -reindex onto the same tip.

The kernel ID and the kernel root are computed over the kernel bytes without the empty field; a block
whose kernel root covers the empty field is invalid.
"""

import copy
import hashlib
import struct
from decimal import Decimal

from test_framework.blocktools import NORMAL_GBT_REQUEST_PARAMS, create_coinbase
from test_framework.fec_util import setup_mweb_chain
from test_framework.key import SECP256K1, SECP256K1_FIELD_SIZE, SECP256K1_G, SECP256K1_ORDER, jacobi_symbol
from test_framework.messages import (
    CBlock,
    CTransaction,
    FromHex,
    Hash,
    MWEBBlock,
    MWEBKernel,
    blake3,
    ser_compact_size,
)
from test_framework.script_util import hogaddr_script
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

PEGOUT_FEATURE_BIT = 0x04
EXTRA_DATA_FEATURE_BIT = 0x20
# The kernel feature rules start far beyond the blocks this test mines.
VBPARAMS = "-vbparams=mwebkernel:0:0:{}:{}".format(1000 * 144, 99999999)


def committed_bytes(kernel):
    """Kernel bytes the kernel ID and the kernel MMR leaf are computed over (Kernel::Serialized): an
    empty pegout list or empty extra data is left out even when its feature bit is set."""
    written = copy.deepcopy(kernel)
    if not written.pegouts:
        written.features &= ~PEGOUT_FEATURE_BIT
    if not written.extradata:
        written.features &= ~EXTRA_DATA_FEATURE_BIT
    return bytes([kernel.features]) + written.serialize()[1:]


def kernel_order(kernel):
    """Sort key of libmw's KernelSort: highest supply change first, then by kernel ID."""
    supply_change = (kernel.pegin or 0) - (kernel.fee or 0) - sum(p.amount for p in kernel.pegouts or [])
    return (-supply_change, blake3(committed_bytes(kernel)).serialize())


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


def point(secret):
    return SECP256K1.affine(SECP256K1.mul([(SECP256K1_G, secret)]))


def sign(secret, msg):
    """Schnorr signature in the form libmw creates and verifies (secp256k1-zkp schnorrsig): R = k*G
    with a square y coordinate, e = SHA256(R.x || compressed public key || msg), s = k + e*secret."""
    k = int.from_bytes(hashlib.sha256(secret.to_bytes(32, 'big') + msg).digest(), 'big') % SECP256K1_ORDER
    R = point(k)
    if jacobi_symbol(R[1], SECP256K1_FIELD_SIZE) != 1:
        k = SECP256K1_ORDER - k
    P = point(secret)
    pubkey = bytes([0x02 + (P[1] & 1)]) + P[0].to_bytes(32, 'big')
    e = int.from_bytes(hashlib.sha256(R[0].to_bytes(32, 'big') + pubkey + msg).digest(), 'big') % SECP256K1_ORDER
    return R[0].to_bytes(32, 'big') + ((k + e * secret) % SECP256K1_ORDER).to_bytes(32, 'big')


def make_kernel(feature, secret):
    """A signed kernel that sets only the given feature bit, with an empty field for it. Its excess
    commits to the value 0 with the blinding factor secret."""
    P = point(secret)
    excess = bytes([0x08 if jacobi_symbol(P[1], SECP256K1_FIELD_SIZE) == 1 else 0x09]) + P[0].to_bytes(32, 'big')
    kernel = MWEBKernel()
    kernel.features = feature
    kernel.pegouts = [] if feature == PEGOUT_FEATURE_BIT else None
    kernel.extradata = b"" if feature == EXTRA_DATA_FEATURE_BIT else None
    kernel.excess = int.from_bytes(excess, 'little')
    # Kernel::GetSignatureMessage leaves out the empty field.
    message = blake3(bytes([feature]) + excess).serialize()
    kernel.signature = int.from_bytes(sign(secret, message), 'little')
    kernel.rehash()
    return kernel, excess


class MWEBKernelRoundTripTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [[VBPARAMS], [VBPARAMS]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        setup_mweb_chain(node)
        self.sync_blocks()
        assert not node.getblockchaininfo()['softforks']['mwebkernel']['active']

        blocks = []
        for feature, secret in ((PEGOUT_FEATURE_BIT, 1), (EXTRA_DATA_FEATURE_BIT, 2)):
            self.log.info("Node 0 accepts a block whose kernel sets feature {:#04x} with an empty field".format(feature))
            block, kernel, excess = self.build_block(feature, secret)
            assert_equal(node.submitblock(block.serialize().hex()), None)
            assert_equal(node.getbestblockhash(), block.hash)
            blocks.append(block)

            mweb = node.getblock(block.hash, 2)['mweb']
            reported = next(k for k in mweb['kernels'] if k['excess'] == excess.hex())
            assert_equal(reported['features'], feature)
            assert_equal(reported['kernel_id'], blake3(committed_bytes(kernel)).to_hex())
            assert kernel.serialize() != committed_bytes(kernel)

            self.check_blocks_read_back(node, blocks)

        self.log.info("Node 1 downloads both blocks from node 0")
        self.sync_blocks()
        self.check_blocks_read_back(self.nodes[1], blocks)

        self.log.info("Node 0 restarts and checks its latest blocks at the highest level")
        self.restart_node(0, extra_args=[VBPARAMS, "-checkblocks=4", "-checklevel=4"])
        assert_equal(node.getbestblockhash(), blocks[-1].hash)
        self.check_blocks_read_back(node, blocks)

        self.log.info("Node 0 rebuilds its block index and chain state from its block files")
        tip_height = node.getblockcount()
        self.restart_node(0, extra_args=[VBPARAMS, "-reindex"])
        self.wait_until(lambda: node.getblockcount() == tip_height)
        assert_equal(node.getbestblockhash(), blocks[-1].hash)
        self.check_blocks_read_back(node, blocks)

    def check_blocks_read_back(self, node, blocks):
        for block in blocks:
            assert_equal(node.getblock(block.hash, 0), block.serialize().hex())

    def build_block(self, feature, secret):
        """Adds the kernel to the next template, which also holds a wallet MWEB transaction, and
        returns the solved block. First proposes the same block with its kernel root computed over
        the bytes that include the empty field, which must fail."""
        node = self.nodes[0]
        node.sendtoaddress(node.getnewaddress(address_type='mweb'), Decimal("0.1"))
        gbt = node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
        mweb_block = FromHex(MWEBBlock(), gbt['mweb'])
        header = mweb_block.header
        kernels = mweb_block.body.kernels
        assert len(kernels) > 0
        assert_equal(mmr_root([committed_bytes(k) for k in kernels]), header.kernel_root.serialize())
        assert_equal(sorted(kernels, key=kernel_order), kernels)

        kernel, excess = make_kernel(feature, secret)
        kernels.append(kernel)
        kernels.sort(key=kernel_order)
        header.num_kernels += 1
        # The kernel's excess is secret*G, so the block's total kernel offset goes down by secret.
        offset = int.from_bytes(header.kernel_offset.serialize(), 'big')
        header.kernel_offset = Hash.from_byte_arr(((offset - secret) % SECP256K1_ORDER).to_bytes(32, 'big'))

        header.kernel_root = Hash.from_byte_arr(mmr_root([k.serialize() for k in kernels]))
        mweb_block.rehash()
        assert_equal(node.getblocktemplate(template_request={
            'data': self.assemble(gbt, mweb_block).serialize().hex(),
            'mode': 'proposal',
            'rules': ['segwit'],
        }), 'bad-blk-mweb')

        header.kernel_root = Hash.from_byte_arr(mmr_root([committed_bytes(k) for k in kernels]))
        mweb_block.rehash()
        block = self.assemble(gbt, mweb_block)
        block.solve()
        return block, kernel, excess

    def assemble(self, gbt, mweb_block):
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
        block.mweb_block = copy.deepcopy(mweb_block)
        block.hashMerkleRoot = block.calc_merkle_root()
        block.rehash()
        return block


if __name__ == '__main__':
    MWEBKernelRoundTripTest().main()
