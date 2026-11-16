#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a PSBT is rejected when its non-witness UTXO lacks the output its input spends.

A PSBT input can carry the whole transaction that created the output it spends (the
non-witness UTXO). Decoding a PSBT checks that this transaction has the hash of the input's
prevout and that it has an output at the prevout's index.

The test builds PSBTs whose non-witness UTXO is a transaction without MWEB data, the same
transaction with MWEB data, the same transaction marked as an integrating (HogEx) transaction,
and an MWEB-only transaction, which has no canonical outputs. A PSBT whose input spends the last
output of the non-witness UTXO decodes. A PSBT whose input spends the output after the last one
is rejected by every PSBT RPC with error -22, and the node keeps running.
"""

import base64

from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    MWEBKernel,
    MWEBTransaction,
    ser_string,
)
from test_framework.script import CScript, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

INDEX_ERROR = "TX decode failed Input specifies output index that does not exist"


def make_psbt(prev_tx, prev_txid, index):
    """Return a PSBT with one input, which spends output index of prev_tx and carries prev_tx
    as its non-witness UTXO, and one output of 1 FEC."""
    tx = CTransaction()
    tx.vin = [CTxIn(COutPoint(prev_txid, index))]
    tx.vout = [CTxOut(1 * COIN, CScript([OP_TRUE]))]
    psbt = b"psbt\xff"
    psbt += ser_string(b"\x00") + ser_string(tx.serialize_without_witness())  # unsigned transaction
    psbt += b"\x00"
    psbt += ser_string(b"\x00") + ser_string(prev_tx.serialize())  # non-witness UTXO, with any MWEB data
    psbt += b"\x00"
    psbt += b"\x00"  # empty output map
    return base64.b64encode(psbt).decode()


def mweb_data():
    """Return MWEB transaction data with one kernel, and the kernel's hash."""
    kernel = MWEBKernel()
    kernel.excess = int.from_bytes(b"\x08" + b"\x11" * 32, "little")
    kernel.signature = int.from_bytes(b"\x22" * 64, "little")
    kernel.rehash()
    mweb_tx = MWEBTransaction()
    mweb_tx.body.kernels = [kernel]
    mweb_tx.rehash()
    return mweb_tx, kernel.hash.val


class PSBTPrevoutIndexTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def assert_rejected(self, psbt):
        node = self.nodes[0]
        assert_raises_rpc_error(-22, INDEX_ERROR, node.decodepsbt, psbt)
        assert_raises_rpc_error(-22, INDEX_ERROR, node.analyzepsbt, psbt)
        assert_raises_rpc_error(-22, INDEX_ERROR, node.combinepsbt, [psbt])
        assert_raises_rpc_error(-22, INDEX_ERROR, node.finalizepsbt, psbt)
        assert_raises_rpc_error(-22, INDEX_ERROR, node.joinpsbts, [psbt, psbt])
        assert_raises_rpc_error(-22, INDEX_ERROR, node.utxoupdatepsbt, psbt)
        if self.is_wallet_compiled():
            assert_raises_rpc_error(-22, INDEX_ERROR, node.walletprocesspsbt, psbt)

    def run_test(self):
        node = self.nodes[0]

        prev_tx = CTransaction()
        prev_tx.vin = [CTxIn(COutPoint(0x01, 0))]
        prev_tx.vout = [CTxOut(2 * COIN, CScript([OP_TRUE])), CTxOut(3 * COIN, CScript([OP_TRUE]))]
        prev_tx.rehash()

        mixed_tx = CTransaction(prev_tx)
        mixed_tx.mweb_tx, _ = mweb_data()

        hogex_tx = CTransaction(prev_tx)
        hogex_tx.hogex = True

        for name, tx in (("without MWEB data", prev_tx),
                         ("with MWEB data", mixed_tx),
                         ("marked as an integrating transaction", hogex_tx)):
            self.log.info("Non-witness UTXO {}: the last output can be spent".format(name))
            decoded = node.decodepsbt(make_psbt(tx, prev_tx.sha256, len(prev_tx.vout) - 1))
            assert_equal(decoded["fee"], 2)
            assert_equal("vkern" in decoded["inputs"][0]["non_witness_utxo"], tx.mweb_tx is not None)

            self.log.info("Non-witness UTXO {}: the output after the last one is rejected".format(name))
            self.assert_rejected(make_psbt(tx, prev_tx.sha256, len(prev_tx.vout)))

        self.log.info("MWEB-only non-witness UTXO: output 0 is rejected")
        mweb_only_tx = CTransaction()
        mweb_only_tx.mweb_tx, kernel_hash = mweb_data()
        self.assert_rejected(make_psbt(mweb_only_tx, kernel_hash, 0))

        self.log.info("The node is still running")
        assert_equal(node.getblockcount(), 0)


if __name__ == '__main__':
    PSBTPrevoutIndexTest().main()
